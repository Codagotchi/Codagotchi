# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| `main` branch | Yes |

## Credential handling

Codagotchi never stores or transmits your API credentials to any third-party service:

- **Claude Code token** — read from the macOS Keychain (`Claude Code-credentials`) at runtime. Never written to disk by the daemon.
- **Codex token** — read from `~/.codex/auth.json`, which the official Codex CLI manages. The daemon refreshes it in-place using atomic writes.
- **BLE traffic** — only JSON usage data (percentages, reset countdowns, active state) is sent to the device. Credentials are never sent over Bluetooth.

## Reporting a vulnerability

If you discover a security issue — especially anything involving credential leakage, BLE eavesdropping, or local privilege escalation — please **do not open a public issue**.

Instead, report it privately via [GitHub's private vulnerability reporting](https://github.com/rachelworld/codagotchi/security/advisories/new).

Include:
- A description of the vulnerability and its potential impact
- Steps to reproduce
- Any suggested fix if you have one

You can expect an acknowledgement within a few days and a fix or mitigation plan within a reasonable timeframe depending on severity.
