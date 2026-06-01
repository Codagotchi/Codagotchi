#!/usr/bin/env python3
"""Codagotchi Daemon (BLE) — macOS daemon for the Codagotchi ESP32 display.

Polls per-provider rate-limit headers (Claude Code, OpenAI Codex) and writes
JSON payloads to the ESP32 "Codagotchi" peripheral over a custom GATT
service. Uses bleak (CoreBluetooth backend on macOS).

Payload schema:
    {"p": "claude"|"codex", "s": <5h_pct>, "sr": <mins>,
     "w": <weekly_pct>, "wr": <mins>, "st": <status>, "ok": true, "ac": <bool>}

Heartbeat (active-state-only) payload:
    {"p": "claude"|"codex", "ac": <bool>}
"""

import abc
import asyncio
import base64
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Codagotchi"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60       # seconds between full API polls (per provider)
STATE_TICK = 0.5         # seconds between active-state checks (fast)
SCAN_TIMEOUT = 8.0

SAVED_ADDR_FILE = Path.home() / ".config" / "codagotchi" / "ble-address"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ======================================================================
# Provider abstraction
# ======================================================================

@dataclass
class PollResult:
    """A successful poll result. Fields map 1:1 to the firmware schema."""
    session_pct: int
    session_reset_mins: int
    weekly_pct: int
    weekly_reset_mins: int
    status: str
    active: bool

    def to_payload(self, provider_name: str) -> dict:
        return {
            "p": provider_name,
            "s": self.session_pct,
            "sr": self.session_reset_mins,
            "w": self.weekly_pct,
            "wr": self.weekly_reset_mins,
            "st": self.status,
            "ok": True,
            "ac": self.active,
        }


class Provider(abc.ABC):
    """One source of usage data (Claude Code, Codex, ...)."""

    name: str = ""

    @abc.abstractmethod
    async def poll(self, http: httpx.AsyncClient) -> PollResult | None:
        """Hit the provider's API and return current usage, or None on error."""

    async def refresh_active(self, http: httpx.AsyncClient) -> None:
        """Optional periodic hook to refresh cached active state.

        Providers that learn active-state from HTTP (e.g. Codex /tasks) update
        their cache here. Providers that learn from local files (Claude
        /tmp/claude_state) leave this as a no-op and do the work in
        is_active() — it's basically free for them.
        """
        return None

    def is_active(self) -> bool:
        """True when the provider's CLI is currently processing on host. Default: no signal."""
        return False


# ----------------------------------------------------------------------
# Claude Code provider
# ----------------------------------------------------------------------

# macOS: token lives in Keychain (service "Claude Code-credentials").
CLAUDE_KEYCHAIN_SERVICE = "Claude Code-credentials"
CLAUDE_API_URL = "https://api.anthropic.com/v1/messages"
CLAUDE_API_HEADERS = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
CLAUDE_API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

_CLAUDE_STATE_FILE = Path("/tmp/claude_state")
_CLAUDE_STATE_EXPIRY_S = 90  # mirrors statusline.sh


def _extract_claude_token(blob: str) -> str | None:
    """Pull accessToken from a credentials blob (JSON or raw)."""
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_claude_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security", "find-generic-password",
                "-s", CLAUDE_KEYCHAIN_SERVICE,
                "-a", getpass.getuser(),
                "-w",
            ],
            check=True, capture_output=True, text=True, timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"[claude] Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"[claude] Keychain access error: {e}")
        return None
    return _extract_claude_token(out.stdout)


def _read_claude_token() -> str | None:
    return _read_claude_token_keychain()


class _ClaudeStateWatcher:
    """Watches /tmp/claude_state. Hooks write 'thinking:<ts>' or 'ready'."""

    def __init__(self) -> None:
        self._mtime: float = -1.0
        self._content: str = ""
        self._ts: int | None = None

    def _refresh(self) -> None:
        try:
            mtime = _CLAUDE_STATE_FILE.stat().st_mtime
        except OSError:
            if self._mtime != -2:
                self._mtime = -2
                self._content = "ready"
                self._ts = None
            return
        if mtime == self._mtime:
            return
        try:
            self._content = _CLAUDE_STATE_FILE.read_text().strip()
        except OSError:
            self._content = "ready"
        self._mtime = mtime
        if self._content.startswith("thinking:"):
            try:
                self._ts = int(self._content[len("thinking:"):])
            except ValueError:
                self._ts = None
        else:
            self._ts = None

    def poll(self) -> bool:
        self._refresh()
        if self._ts is not None:
            return (time.time() - self._ts) <= _CLAUDE_STATE_EXPIRY_S
        return False


class ClaudeProvider(Provider):
    name = "claude"

    def __init__(self) -> None:
        self._watcher = _ClaudeStateWatcher()

    def is_active(self) -> bool:
        return self._watcher.poll()

    async def poll(self, http: httpx.AsyncClient) -> PollResult | None:
        token = _read_claude_token()
        if not token:
            log("[claude] No token; skipping poll")
            return None
        headers = dict(CLAUDE_API_HEADERS)
        headers["Authorization"] = f"Bearer {token}"
        try:
            resp = await http.post(CLAUDE_API_URL, headers=headers, json=CLAUDE_API_BODY)
        except httpx.HTTPError as e:
            log(f"[claude] API call failed: {e}")
            return None
        if resp.status_code >= 400:
            log(f"[claude] HTTP {resp.status_code}: {resp.text[:200]}")
            return None

        now = time.time()

        def hdr(name: str, default: str = "0") -> str:
            return resp.headers.get(name, default)

        def reset_minutes(ts: str) -> int:
            try:
                r = float(ts)
            except ValueError:
                return 0
            mins = (r - now) / 60.0
            return int(round(mins)) if mins > 0 else 0

        def pct(util: str) -> int:
            try:
                return int(round(float(util) * 100))
            except ValueError:
                return 0

        return PollResult(
            session_pct=pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            session_reset_mins=reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            weekly_pct=pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            weekly_reset_mins=reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            status=hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            active=self.is_active(),
        )


# ----------------------------------------------------------------------
# OpenAI Codex provider (ChatGPT-subscription auth)
# ----------------------------------------------------------------------

# Codex auth lives in ~/.codex/auth.json, structured as:
#   {"tokens": {"access_token": "<JWT>", "refresh_token": "rt_...",
#               "account_id": "<chatgpt account uuid>"},
#    "last_refresh": "..."}
#
# We use two ChatGPT-backend endpoints (both undocumented, both used by the
# Codex CLI and IDE extension):
#
#   GET /backend-api/codex/usage  -> rate-limit JSON
#     { "plan_type": "...", "rate_limit": {
#         "primary_window":   { "used_percent": N, "reset_after_seconds": N },
#         "secondary_window": { "used_percent": N, "reset_after_seconds": N } } }
#     Cleaner than scraping x-codex-* response headers off a streamed
#     /responses probe — no token cost, no model-name dance.
#
#   GET /backend-api/codex/tasks  -> running cloud Codex tasks
#     { "items": [...], "cursor": null }
#     Empty items list = nothing running. Used as the "active" signal.
#     Caveat: only catches cloud tasks (chatgpt.com/codex). Local CLI runs
#     don't register here.

CODEX_AUTH_PATH = Path.home() / ".codex" / "auth.json"
CODEX_SESSIONS_DIR = Path.home() / ".codex" / "sessions"
CODEX_USAGE_URL = "https://chatgpt.com/backend-api/codex/usage"
CODEX_TASKS_URL = "https://chatgpt.com/backend-api/codex/tasks"
CODEX_REFRESH_URL = "https://auth.openai.com/oauth/token"
# Pulled from the JWT `aud` claim — same client_id the Codex CLI registers as.
CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"

# Refresh slightly before actual expiry so concurrent polls don't 401.
CODEX_REFRESH_SKEW_S = 300

# How often to re-poll /tasks for the active-state cache. Heartbeats run
# every STATE_TICK (0.5s) — much too fast to hit OpenAI on every tick.
CODEX_ACTIVE_REFRESH_S = 10

# Local CLI activity: how recent a session file write must be to count as
# "Codex is processing right now". Codex CLI writes to its rollout-*.jsonl
# on every model response chunk, so 5s of inactivity is a safe idle signal.
CODEX_LOCAL_STALE_S = 5


def _decode_jwt_exp(jwt: str) -> int | None:
    """Return the `exp` claim (unix seconds) from a JWT, or None if unparseable."""
    try:
        _, payload_b64, _ = jwt.split(".")
        # JWTs use URL-safe base64 without padding.
        padding = "=" * (-len(payload_b64) % 4)
        payload = json.loads(base64.urlsafe_b64decode(payload_b64 + padding))
    except (ValueError, json.JSONDecodeError):
        return None
    exp = payload.get("exp")
    return int(exp) if isinstance(exp, (int, float)) else None


class CodexProvider(Provider):
    name = "codex"

    def __init__(self) -> None:
        self._auth: dict | None = None
        self._auth_mtime: float | None = None
        # Cached active-state. Combined from two signals:
        #   - cloud: /backend-api/codex/tasks — cloud Codex tasks in flight
        #   - local: any session rollout-*.jsonl mtime within the last few
        #            seconds — Codex CLI actively streaming responses
        # /tasks is throttled to one HTTP call per CODEX_ACTIVE_REFRESH_S;
        # the local check is just a stat() so we run it every tick.
        self._cloud_active: bool = False
        self._local_active: bool = False
        self._active_cached: bool = False
        self._cloud_last_check: float = 0.0

    # ----- auth.json I/O -----

    def _load_auth(self) -> dict | None:
        try:
            st = CODEX_AUTH_PATH.stat()
        except OSError:
            return None
        if self._auth is None or self._auth_mtime != st.st_mtime:
            try:
                self._auth = json.loads(CODEX_AUTH_PATH.read_text())
            except (OSError, json.JSONDecodeError) as e:
                log(f"[codex] Auth read failed: {e}")
                return None
            self._auth_mtime = st.st_mtime
        return self._auth

    def _save_auth(self, auth: dict) -> None:
        # Atomic rewrite so a partial write can't corrupt the file the Codex CLI also reads.
        tmp = CODEX_AUTH_PATH.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(auth, indent=2))
        os.chmod(tmp, 0o600)
        tmp.replace(CODEX_AUTH_PATH)
        self._auth = auth
        self._auth_mtime = CODEX_AUTH_PATH.stat().st_mtime

    # ----- token refresh -----

    async def _refresh_token(self, http: httpx.AsyncClient, refresh_token: str) -> dict | None:
        body = {
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": CODEX_CLIENT_ID,
            "scope": "openid profile email offline_access",
        }
        try:
            resp = await http.post(CODEX_REFRESH_URL, json=body)
        except httpx.HTTPError as e:
            log(f"[codex] Token refresh failed: {e}")
            return None
        if resp.status_code >= 400:
            log(f"[codex] Refresh HTTP {resp.status_code}: {resp.text[:200]}")
            return None
        try:
            return resp.json()
        except json.JSONDecodeError:
            log("[codex] Refresh returned non-JSON")
            return None

    async def _ensure_fresh_token(self, http: httpx.AsyncClient) -> str | None:
        """Return a valid access_token, refreshing if it's expired or near expiry."""
        auth = self._load_auth()
        if not auth:
            log("[codex] auth.json not found — is Codex logged in?")
            return None
        tokens = auth.get("tokens") or {}
        access = tokens.get("access_token")
        refresh = tokens.get("refresh_token")
        if not access or not refresh:
            log("[codex] auth.json missing access_token or refresh_token")
            return None

        exp = _decode_jwt_exp(access)
        now = int(time.time())
        if exp is None or (exp - now) <= CODEX_REFRESH_SKEW_S:
            log("[codex] Access token stale, refreshing...")
            refreshed = await self._refresh_token(http, refresh)
            if not refreshed:
                return None
            access = refreshed.get("access_token") or access
            id_token = refreshed.get("id_token") or tokens.get("id_token")
            new_refresh = refreshed.get("refresh_token") or refresh
            tokens.update({
                "access_token": access,
                "id_token": id_token,
                "refresh_token": new_refresh,
            })
            auth["tokens"] = tokens
            auth["last_refresh"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            try:
                self._save_auth(auth)
            except OSError as e:
                log(f"[codex] Could not persist refreshed auth: {e}")
        return access

    # ----- HTTP helpers -----

    async def _authed_get(
        self, http: httpx.AsyncClient, url: str
    ) -> httpx.Response | None:
        """GET with ChatGPT auth headers, refreshing the token first."""
        access = await self._ensure_fresh_token(http)
        if not access:
            return None
        auth = self._load_auth() or {}
        account_id = (auth.get("tokens") or {}).get("account_id") or ""
        headers = {
            "Authorization": f"Bearer {access}",
            "chatgpt-account-id": account_id,
            "Accept": "application/json",
            "User-Agent": "codex_cli_rs",
            "originator": "codex_cli_rs",
        }
        try:
            return await http.get(url, headers=headers)
        except httpx.HTTPError as e:
            log(f"[codex] GET {url} failed: {e}")
            return None

    # ----- active state: cloud /tasks ∪ local session-file mtime -----

    def _newest_session_mtime(self) -> float:
        """Return the highest mtime across recent codex session files, or 0.

        Sessions are at ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl. We scan
        today and yesterday (for the midnight crossover case) — at most two
        small directories. The Codex CLI writes to its active rollout file
        on every model response chunk, so the mtime is a faithful "doing
        work right now" signal.
        """
        if not CODEX_SESSIONS_DIR.exists():
            return 0.0
        from datetime import datetime, timedelta
        now_dt = datetime.now()
        best = 0.0
        for offset in (0, 1):
            d = now_dt - timedelta(days=offset)
            ddir = CODEX_SESSIONS_DIR / d.strftime("%Y") / d.strftime("%m") / d.strftime("%d")
            if not ddir.exists():
                continue
            try:
                for entry in ddir.iterdir():
                    name = entry.name
                    if name.startswith("rollout-") and name.endswith(".jsonl"):
                        try:
                            m = entry.stat().st_mtime
                            if m > best:
                                best = m
                        except OSError:
                            pass
            except OSError:
                pass
        return best

    async def refresh_active(self, http: httpx.AsyncClient) -> None:
        now = time.time()

        # Local CLI signal — cheap, run every tick.
        latest = self._newest_session_mtime()
        self._local_active = latest > 0 and (now - latest) <= CODEX_LOCAL_STALE_S

        # Cloud signal — HTTP, throttled.
        if now - self._cloud_last_check >= CODEX_ACTIVE_REFRESH_S:
            self._cloud_last_check = now
            resp = await self._authed_get(http, CODEX_TASKS_URL)
            if resp is not None and resp.status_code < 400:
                try:
                    data = resp.json()
                    items = data.get("items") if isinstance(data, dict) else None
                    self._cloud_active = bool(items)
                except json.JSONDecodeError:
                    pass  # leave previous value

        self._active_cached = self._cloud_active or self._local_active

    def is_active(self) -> bool:
        return self._active_cached

    # ----- usage poll -----

    async def poll(self, http: httpx.AsyncClient) -> PollResult | None:
        resp = await self._authed_get(http, CODEX_USAGE_URL)
        if resp is None:
            return None
        if resp.status_code >= 400:
            log(f"[codex] /usage HTTP {resp.status_code}: {resp.text[:200]}")
            return None
        try:
            data = resp.json()
        except json.JSONDecodeError:
            log("[codex] /usage returned non-JSON")
            return None

        rl = data.get("rate_limit") or {}
        primary = rl.get("primary_window") or {}
        secondary = rl.get("secondary_window") or {}

        def pct(v) -> int:
            try:
                return int(round(float(v or 0)))
            except (TypeError, ValueError):
                return 0

        def mins(v) -> int:
            try:
                return max(0, int(round(float(v or 0) / 60.0)))
            except (TypeError, ValueError):
                return 0

        # Status: prefer the explicit `allowed`/`limit_reached` flags; fall
        # back to utilization if neither is present.
        if rl.get("limit_reached") is True or rl.get("allowed") is False:
            status = "limited"
        else:
            status = "allowed"

        return PollResult(
            session_pct=pct(primary.get("used_percent")),
            session_reset_mins=mins(primary.get("reset_after_seconds")),
            weekly_pct=pct(secondary.get("used_percent")),
            weekly_reset_mins=mins(secondary.get("reset_after_seconds")),
            status=status,
            active=self.is_active(),
        )


# ======================================================================
# BLE address discovery
# ======================================================================

def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# ======================================================================
# BLE session
# ======================================================================

class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


# ======================================================================
# Main per-connection loop
# ======================================================================

async def connect_and_run(
    address: str,
    providers: list[Provider],
    stop_event: asyncio.Event,
) -> bool:
    """Connect, then poll all providers until disconnected or stopped."""
    log(f"Connecting to {address}...")
    client = BleakClient(address)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False
    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    # Per-provider bookkeeping. None for last_ac forces a heartbeat on first tick.
    last_poll: dict[str, float] = {p.name: 0.0 for p in providers}
    last_ac: dict[str, bool | None] = {p.name: None for p in providers}
    used_successfully = False

    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            while client.is_connected and not stop_event.is_set():
                now = time.time()

                # --- Per-provider active heartbeats ---
                # refresh_active() is the hook for providers that learn
                # active-state from HTTP (Codex /tasks). It's internally
                # rate-limited so calling it every tick is cheap. Providers
                # backed by a local file watcher (Claude) leave it as a no-op.
                for p in providers:
                    await p.refresh_active(http)
                    ac = p.is_active()
                    if ac != last_ac[p.name]:
                        prev = "?" if last_ac[p.name] is None else last_ac[p.name]
                        log(f"[{p.name}] Active state: {prev} -> {ac}")
                        await session.write_payload({"p": p.name, "ac": ac})
                        last_ac[p.name] = ac

                # --- Full polls (one per provider, when due) ---
                refresh_now = session.refresh_requested.is_set()
                if refresh_now:
                    session.refresh_requested.clear()
                for p in providers:
                    if refresh_now or now - last_poll[p.name] >= POLL_INTERVAL:
                        result = await p.poll(http)
                        if result is not None:
                            payload = result.to_payload(p.name)
                            if await session.write_payload(payload):
                                last_poll[p.name] = time.time()
                                last_ac[p.name] = result.active
                                used_successfully = True

                # Wake immediately if the device fires a refresh notify.
                try:
                    await asyncio.wait_for(
                        session.refresh_requested.wait(), timeout=STATE_TICK
                    )
                except asyncio.TimeoutError:
                    pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


# ======================================================================
# Entry
# ======================================================================

def _enabled_providers() -> list[Provider]:
    """Decide which providers to run. Driven by CODAGOTCHI_PROVIDERS env var
    (comma-separated: "claude,codex") or auto-detect from credential presence."""
    env = os.environ.get("CODAGOTCHI_PROVIDERS", "").strip()
    if env:
        names = [n.strip() for n in env.split(",") if n.strip()]
    else:
        names = []
        # Auto-detect: Claude always (token read from the macOS Keychain); Codex
        # if ~/.codex/auth.json exists.
        names.append("claude")
        if CODEX_AUTH_PATH.exists():
            names.append("codex")

    providers: list[Provider] = []
    for n in names:
        if n == "claude":
            providers.append(ClaudeProvider())
        elif n == "codex":
            providers.append(CodexProvider())
        else:
            log(f"Unknown provider '{n}', skipping")
    return providers


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    providers = _enabled_providers()
    log("=== Codagotchi Daemon (BLE) ===")
    log(f"Providers: {', '.join(p.name for p in providers)}")
    log(f"Poll interval: {POLL_INTERVAL}s per provider")

    backoff = 1
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(address, providers, stop_event)
        if not ok:
            log("Invalidating cached address")
            SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
