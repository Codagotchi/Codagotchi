# Contributing to Codagotchi

Contributions are welcome — new boards, new pets, new providers, bug fixes, and daemon improvements.

## Before you start

- Check [open issues](https://github.com/rachelworld/codagotchi/issues) to avoid duplicating work.
- For large changes (new provider, new UI screen, architecture refactor), open an issue first to discuss approach before writing code.
- For new boards, read [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md) and the [HAL contract](docs/porting/hal-contract.md) before starting.

## What to contribute

| Type | Where to look |
|------|---------------|
| New board port | `docs/porting/adding-a-board.md` |
| New pet / sprite | README "Customizing the pet" section |
| New provider | `daemon/codagotchi_daemon.py` + firmware data model |
| Bug fix | Open an issue, then a PR |
| UI / layout tweak | `firmware/src/ui.cpp`, `splash.cpp` |
| Daemon improvement | `daemon/codagotchi_daemon.py` |

## Art and asset licensing

The pets and fonts shipped in this repo are **placeholders under restrictive licenses** and cannot be redistributed. Before contributing art:

- Only submit assets you created or that are under a permissive license (MIT, Apache, CC BY, CC0, etc.).
- Include the license in your PR description.
- If you're replacing a placeholder, note which file it replaces.

## Code style

- **No `#ifdef BOARD_*` in shared code** (`main.cpp`, `ui.cpp`, `splash.cpp`, `ble.cpp`). Put board-specific logic in `boards/<name>/` and expose it through the HAL. See [`docs/porting/capability-flags.md`](docs/porting/capability-flags.md).
- Keep `main.cpp` calling only HAL functions — no direct hardware register access.
- Match the surrounding code's style. No enforced formatter currently; just be consistent.
- Comments only when the *why* is non-obvious. Don't describe what the code does.

## Testing expectations

There are no automated unit tests for the firmware (hardware-in-the-loop only). When submitting firmware changes:

- Flash and test on at least one physical board. State which one in the PR.
- For UI changes, use `./screenshot.sh out.png` to capture a screenshot and attach it to the PR.
- Make sure the project still builds for all envs you didn't physically test: `pio run -d firmware -e <env>`.
- For daemon changes, run the daemon against a live device and confirm the usage display updates correctly.

## Pull request checklist

- [ ] Builds cleanly for all affected PlatformIO envs
- [ ] No `#ifdef BOARD_*` added to shared files
- [ ] Any new assets are under a permissive/CC license (or are your own work)
- [ ] Screenshot attached if UI changed
- [ ] Tested on hardware (state which board)

## Setting up locally

See the [Quick start](README.md#quick-start) section of the README. The firmware requires [PlatformIO](https://platformio.org/) (`brew install platformio`); the daemon requires Python 3.9+ and runs on macOS.

## Reporting bugs

Use the [bug report template](https://github.com/rachelworld/codagotchi/issues/new?template=bug_report.md). Include your board, firmware build env, macOS version, and serial output if relevant.
