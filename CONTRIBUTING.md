# Contributing to ZeroExt

Thank you for considering contributing to **ZeroExt**!

## Development Prerequisites (Windows 10 / 11 x64)

- **Visual Studio 2022** (BuildTools / Community / Professional / Enterprise) with C/C++ workload (`cl.exe`), **CMake (>= 3.20)**, **Ninja**, and **CTest**
- **Python 3.x** (used at CMake configure/build time by `scripts/generate_e2fsprogs_headers.py`)
- **.NET 10 SDK** (optional; required only to build and test `ZeroExtGui.exe` and `ZeroExt.Gui.Tests`)
- **WinFsp Runtime** (not required to build or run automated tests; required only when mounting live drive letters on Windows)

## Setup, Build & Test

1. Clone the repository:
   ```powershell
   git clone https://github.com/totatake/ZeroExt.git
   cd ZeroExt
   ```
2. Run the full native C11 build, CTest suite, GUI build, and GUI test suite:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1
   ```
   Use `-SkipGui` to build and test only the C/CMake targets, or run GUI checks alone:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\build_gui.ps1
   ```
3. Before opening a PR touching C sources (`src/`, `tests/`, `compat/`), run sanitizers and static analysis:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1
   ```
   See [`docs/guides/development.md`](docs/guides/development.md), [`docs/testing.md`](docs/testing.md), and [`docs/guides/usage.md`](docs/guides/usage.md) for details on build prerequisites, suites, and options.

## Architectural & Licensing Rules

1. **GPLv2 & GUI Process Isolation Boundary**: Core binaries linking `libext2fs` (`zeroext_core`, `zeroext.exe`) are distributed under **GPLv2** (see [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)). `gui/ZeroExt.Gui/` **must never** natively link or P/Invoke `zeroext_core` / `ext2fs`; keep all GUI communication decoupled via `zeroext.exe` subprocess invocations and JSON / Named Event IPC.
2. **Zero-Patch `third_party/e2fsprogs/`**: Do not add manual `#ifdef _WIN32` / `#ifdef _MSC_VER` edits to `third_party/e2fsprogs/`. Isolate compiler and POSIX shims inside `compat/`, header generation inside `scripts/generate_e2fsprogs_headers.py`, and reproducible upstream sync adjustments inside `scripts/update_e2fsprogs.ps1`.
3. **Safety First**: Preserve partition boundary checks in `win32_io`, Dirty Journal RW guards, Overlay `.wh.__ext4_lower_id` lower-volume verification, and WinFsp target checks on `unmount`.

## Workflow & Pull Requests

1. Create a topic branch from `main` (`feat/<topic>`, `fix/<topic>`, `perf/<topic>`, `docs/<topic>`).
2. Add or update tests in `tests/` (CTest) and/or `gui/ZeroExt.Gui.Tests/` for behavioral changes.
3. Update relevant documentation under `docs/`:
   - Record significant architectural decisions as numbered files under `docs/adr/` and index them in `docs/adr/README.md`.
   - Record benchmark or Perfetto trace evaluations under `docs/reports/` with commit hash, hardware details, and reproduction commands.
   - Keep `README.md` and `README.ja.md` aligned when user-facing features or flags change, and add a note under `## [Unreleased]` in `CHANGELOG.md`.
4. Prefer [Conventional Commits](https://www.conventionalcommits.org/) (`feat(scope): ...`, `fix(scope): ...`, `perf(scope): ...`, `docs(scope): ...`, `test(scope): ...`, `chore(scope): ...`).
5. Fill out [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md) when submitting your PR.
