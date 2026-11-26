# AGENTS.md — AI Agent Operational Guide for ZeroExt4

This file provides concise repository context, build/test commands, and non-negotiable architectural invariants for automated coding agents. Human-oriented background, comparisons, and use cases live in [`docs/overview.md`](docs/overview.md).

## 1. Repository Map

- `src/cli/` — `zeroext4.exe` CLI entry point (`list`, `inspect`, `mkfs`, `mount`, `unmount`, offline ops, `--json`, `--json-events`)
- `src/driver/` — WinFsp `FSP_FILE_SYSTEM_INTERFACE` callbacks, dynamic `winfsp-x64.dll` loader, UAC session mount-point helper
- `src/vfs/` — Modular VFS operations (`ext4_vfs.h`, `op_init.c` .. `op_flush.c`) and 16-shard dentry cache
- `src/extent/` — Lockless Direct Extent I/O engine & 16-shard `extent_cache` (`O(log N)` binary search)
- `src/overlay/` — Non-destructive NTFS Upperdir Overlay VFS (Copy-up, `.wh.*` whiteouts, `.wh.__ext4_lower_id` guard)
- `src/io/` — Custom Win32 `struct_io_manager` (`FILE_FLAG_OVERLAPPED` + FLS events, 4KB RMW, partition guard, 32-shard CLOCK cache, `win32_io_pread_direct`)
- `src/disk/` — GPT / MBR / EBR partition parser, ext4 superblock & JBD2 journal inspector, disk scanner
- `src/trace/` — Zero-overhead-when-off lockless Perfetto Chrome Trace Event tracer (`--trace`)
- `src/common/` — `libext2fs` / errno / `NTSTATUS` translation helpers and UTF-8/UTF-16 conversion
- `compat/` — Non-invasive MSVC C11 & POSIX header shims (`msvc_compat.h`, `msvc_public_compat.h`, `compat/include/`)
- `third_party/` — Bundled upstream `e2fsprogs` `1.47.4` and standalone WinFsp ABI headers (`winfsp.h`)
- `tests/` — CTest suite (`test_main.c` + 6 suites / 33 tests) and `bench_direct_io.c` (`zeroext4_bench.exe`)
- `gui/` — `.NET 10` WPF desktop manager (`ZeroExt4.Gui`, zero NuGet deps) and automated tests (`ZeroExt4.Gui.Tests`)
- `scripts/` — PowerShell build/test/sanitizer/benchmark/upstream-sync scripts and `generate_e2fsprogs_headers.py`
- `docs/` — Engineering documentation (Japanese default, lowercase `kebab-case`)

## 2. Build, Test & Static Analysis Commands

All native targets require **Windows 10/11 x64** with **Visual Studio 2022** (`cl.exe`, CMake, Ninja, CTest) and **Python 3.x** (plus **.NET 10 SDK** for `ZeroExt4Gui`):

```powershell
# Full native C11 build + CTest + GUI build + GUI tests + --self-test
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1

# Clean C/C++ build without GUI
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1 -Clean -SkipGui

# GUI only (ZeroExt4Gui.exe + ZeroExt4.Gui.Tests + --self-test)
powershell -ExecutionPolicy Bypass -File .\scripts\build_gui.ps1

# Memory safety & static analysis (ASan + MSVC Debug CRT leak check + /analyze)
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1
```

On non-Windows hosts, header generation can still be smoke-tested independently:

```bash
python3 scripts/generate_e2fsprogs_headers.py \
  --et-file third_party/e2fsprogs/lib/ext2fs/ext2_err.et.in \
  --types-file third_party/e2fsprogs/lib/ext2fs/ext2_types.h.in \
  --version-file third_party/e2fsprogs/version.h \
  --out-dir /tmp/zeroext4_gen
```

## 3. Architectural Invariants & Guardrails

1. **Zero-Patch `third_party/e2fsprogs/`**: Never hand-edit `third_party/e2fsprogs/` with Windows-specific `#ifdef`s. Place compiler/POSIX shims in `compat/` and keep reproducible MSVC C11 adjustments strictly inside `scripts/update_e2fsprogs.ps1`.
2. **Strict Lock Hierarchy (Deadlock Prevention)**: Always acquire locks in the fixed order `mut_cs` (Overlay) -> `ctx->rwlock` -> `ctx->fs_lock` -> `win32_io` shard `cache_lock`. Leaf locks (`extent_cache` shards, `overlay->map_lock`) must never call upward into VFS/IO locks; `ext4_extent_pread()` holds no locks across `win32_io_pread_direct()`.
3. **Direct Extent I/O Is Read-Only Lower Only**: Direct Extent I/O is default ON only for Read-Only mounts and `--overlay` (strictly RO Lower). Never enable it for `--rw`; combining `--rw` and `--direct-io` must return `-EINVAL`.
4. **Safety Guards Must Remain Unbypassable by Default**:
   - `win32_io` must always clamp every read/write within `[partition_offset_bytes, partition_offset_bytes + partition_size_bytes)`.
   - Dirty JBD2 journals / `EXT2_ERROR_FS` must block RW mounts unless `--ro-fallback` or explicit `--force-dirty` is given.
   - `--overlay` must keep the lower ext4 volume `GENERIC_READ` and validate `.wh.__ext4_lower_id` before reusing an existing Upperdir.
   - `unmount` and GUI residual cleanup must verify WinFsp/ZeroExt4 ownership via `QueryDosDeviceW` / `GetVolumeInformationW` before removing drive definitions.
5. **GPLv2 & GUI Isolation Boundary**: `zeroext4_core` links GPLv2 `libext2fs`. `gui/ZeroExt4.Gui/` must stay decoupled and communicate strictly via `zeroext4.exe` CLI subprocesses and JSON / Named Event IPC (`--json`, `--json-events`, `--stop-event`).
6. **WinFsp ABI & Handle Lifetime**: Maintain compile-time `FSP_ABI_ASSERT` layout checks in `third_party/winfsp/include/winfsp/winfsp.h` and keep `UmFileContextIsUserContext2 = 1` so every `Open`/`Create` pairs 1:1 with `Close`.
7. **Zero Sanitizer / Static Analysis Regressions**: C/C++ changes (`src/`, `tests/`) compile at `/W4` (selected benign warnings disabled in `CMakeLists.txt`) and must pass ASan, Debug CRT leak checks, and MSVC `/analyze`.

## 4. Documentation Rules

- Root OSS files (`README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `AGENTS.md`, `THIRD_PARTY_NOTICES.md`) are in English; `README.ja.md` is kept in 1:1 section and link-target parity with `README.md`.
- `docs/` uses lowercase `kebab-case` filenames (`README.md` for directory hubs) and Japanese by default.
- Place design decisions in `docs/adr/NNNN-<title>.md` + index in `docs/adr/README.md`, detailed design docs in `docs/design/`, user guides in `docs/guides/`, and benchmark/Perfetto reports in `docs/reports/`.
- Verify links and repository hygiene via `python3 ../skills/repository-structure/scripts/audit_repo.py . --tier full --strict` when available.
