# Changelog

All notable changes to **ZeroExt4** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.10.1] - 2026-11-27

### Added
- Added opt-in `--rw-fast-io` mount option across VFS (`ext4_mount_config_t.enable_rw_fast_io`), CLI (`zeroext4 mount --rw --rw-fast-io`, rejected outside `--rw`), WPF GUI (`MainWindow.xaml` / `MainViewModel` / `ZeroExt4ArgBuilder` / `VolumePreset` / localized strings / `ArgBuilder` tests), and unit tests (`test_rw_batched_extent_write_and_read_coherence`, `test_rw_extent_tree_split_reverse_and_fsck_integrity`, `test_rw_uninit_extent_fallback_and_truncate_stress`, `test_rw_fast_io_multileaf_hole_boundary_write_and_read`, plus deterministic `fsx`-style shadow-buffer fuzzer `test_rw_fast_io_fsx_fuzzer` across 1 KB/4 KB block images; 37 CTest functions total).

### Fixed
- Fixed multi-leaf extent-tree (`eh_depth >= 1`) hole-boundary navigation in `--rw-fast-io` (`src/vfs/op_write.c`, `src/vfs/op_read.c`): replaced `EXT2_EXTENT_NEXT_LEAF` after `ext2fs_extent_goto()` (whose `EXT2_EXTENT_DOWN` leaves interior `visit_num == 0`) with explicit sibling/ancestor traversal (`find_next_leaf_after()`), preventing overlapping extent insertion on cross-leaf hole writes, preventing silent zero-reads across leaf-boundary holes, and unconditionally restoring handle level to `left_ext` on `EXT2_ET_EXTENT_NO_NEXT`.

### Changed
- Restored strict upstream `e2fsprogs` (`ext2fs_file_read` / `ext2fs_file_write`) behavior by default for `--rw` and `--no-direct-io` mounts so custom extent read/write logic is only active when `--rw --rw-fast-io` is explicitly requested (aligned zero-copy `win32_aligned_write`, batched `win32_zeroout`, and 64-slot `icache` remain active within the standard `libext2fs` I/O layer).
- Updated `zeroext4_bench.exe --rw-bench` to benchmark both default `--rw` (`e2fsprogs`: `241.4 MB/s` Seq 1MB write, `887.2 MB/s` 8T read) and `--rw --rw-fast-io` (`4,091.9 MB/s` Seq 1MB write, `32,879.9 MB/s` 8T read), and synchronized documentation and version references to `v0.10.1`.

## [0.10.0] - 2026-11-27

### Added
- Added `--rw-bench` mode to `zeroext4_bench.exe` (`tests/bench_direct_io.c`) and RW integrity/coherence tests (`test_rw_batched_extent_write_and_read_coherence`, `test_rw_extent_tree_split_reverse_and_fsck_integrity`, `test_rw_uninit_extent_fallback_and_truncate_stress`) with `verify_ext4_image_integrity()` in `tests/test_vfs_operations.c` (35 CTest functions total).

### Changed
- Accelerated `--rw` and standard (`--no-direct-io`) extent I/O while keeping Direct Extent I/O strictly RO/Overlay-only: added contiguous extent-batched multi-block writes with error-path `inode_dirty` flushing (`src/vfs/op_write.c`), parallel multi-block standard reads releasing `ctx->fs_lock` across `io_channel_read_blk64` under `ctx->rwlock (Shared)` (`src/vfs/op_read.c`), zero-copy aligned writes and batched `win32_zeroout` (`src/io/win32_io.c`), 64-slot `libext2fs` `icache`, and acquire loads for `cached_extents` / `dentry_cache->epoch` (`src/vfs/op_init.c`, `src/vfs/op_read.c`) — improving Standard Cold NVMe Seq 1MB from `75.2` to `1,433.5 MB/s` (19.1x), WinFsp live mount `--no-direct-io` 1MB Seq from `675.8` to `2,739.8 MB/s` (4.05x), and VFS 8T Seq 64KB from `838.7` to `23,314.0 MB/s` (27.8x), with RW 1MB Seq writes reaching `4,184.5 MB/s`.
- Re-evaluated same-SSD / WinFsp live mount / VFS / `--rw` benchmarks and synchronized version references (`v0.10.0`), supported versions in `SECURITY.md`, issue template examples, and documentation indices across `README.md`, `README.ja.md`, `docs/README.md`, `docs/overview.md`, `docs/roadmap.md`, `docs/testing.md`, `docs/reports/performance-evaluation.md`, and `docs/reports/ext4windows-comparison.md`.

## [0.9.0] - 2026-11-26

### Added
- Added technical source-level comparison report against `Ext4Windows` (`v1.3.0` / `31b83e2`) in [`docs/reports/ext4windows-comparison.md`](docs/reports/ext4windows-comparison.md), linked from `README.md`, `README.ja.md`, `docs/README.md`, `docs/overview.md`, and `docs/reports/README.md`.

### Changed
- **Breaking**: Renamed the project, executables, build targets, CMake options, GUI solution/projects, and settings path from `ZeroExt` to `ZeroExt4` (`zeroext.exe` -> `zeroext4.exe`, `zeroext_bench.exe` -> `zeroext4_bench.exe`, `zeroext_tests.exe` -> `zeroext4_tests.exe`, `zeroext_core` -> `zeroext4_core`, `ZEROEXT_*` -> `ZEROEXT4_*` CMake options, `ZeroExtGui.exe` -> `ZeroExt4Gui.exe`, `gui/ZeroExt.sln` -> `gui/ZeroExt4.sln`, `gui/ZeroExt.Gui*` -> `gui/ZeroExt4.Gui*`, `%LOCALAPPDATA%\ZeroExt\gui_settings.json` -> `%LOCALAPPDATA%\ZeroExt4\gui_settings.json`, `ZeroExt-<tag>-win-x64.zip` -> `ZeroExt4-<tag>-win-x64.zip`, `zeroext_crash.dmp` -> `zeroext4_crash.dmp`).
- Synchronized version references (`v0.9.0`), supported versions in `SECURITY.md`, issue template examples, and documentation indices across `README.md`, `README.ja.md`, `docs/README.md`, `docs/roadmap.md`, `docs/design/detailed-design.md`, and `docs/reports/ext4windows-comparison.md`.

## [0.8.1] - 2026-11-25

### Added
- Added [`docs/guides/README.md`](docs/guides/README.md) hub and [`docs/guides/development.md`](docs/guides/development.md) to separate developer build, `e2fsprogs` upstream sync, benchmark, and Perfetto tracing procedures from end-user instructions.

### Changed
- Standardized H1 titles, `## N.` / `### N.M` section numbering, and 1:1 index link labels across `docs/` and root hubs (`docs/README.md`, `docs/adr/README.md`, `docs/design/README.md`, `docs/reports/README.md`).
- Migrated requirement IDs in `docs/requirements.md` (and `ADR-0010`) from `FR-*` / `NFR-*` to `REQ-F-001..011` / `REQ-NF-001..004`, reflecting `ZeroExt4Gui.exe`, `--case-sensitive`, and safe `unmount`.
- Streamlined `README.md` and `README.ja.md` with strict 1:1 section/link parity and synchronized benchmark figures in `README.md`, `README.ja.md`, and `docs/overview.md` to the `78cd744` (`e2fsprogs 1.47.4`) evaluation.
- Updated `docs/architecture.md` (added `ZeroExt4Gui.exe` IPC boundary and `src/trace/`), `docs/design/detailed-design.md` (32-shard CLOCK / 16-shard `extent_cache` / `cached_extents`), `docs/roadmap.md` (removed duplicated completed sections and updated `ADR-0013` anchor), and `docs/reports/perfetto-trace-analysis.md` (reframed as a completed optimization report).

## [0.8.0] - 2026-11-23

### Added
- Added `--case-sensitive` mount mode in CLI (`zeroext4 mount`) and `ZeroExt4Gui.exe` Advanced Options, enabling NTFS per-directory `FileCaseSensitiveInfo` on `--overlay` upperdirs and its subdirectories.
- Added `:cs` suffix to `.wh.__ext4_lower_id` markers in case-sensitive overlay upperdirs to block accidental mixing with case-insensitive mounts.
- Added pre-mount upperdir validation (`IsDirectoryCaseSensitive`), `_cs` suffix in suggested overlay paths, and translated GUI error diagnostics (`winerr=145`, `winerr=5`, unsupported volumes) in `MountErrorTranslator`.
- Added native unit tests (`test_case_sensitive_coexistence`, `test_overlay_case_sensitive_upperdir`) and GUI tests for `--case-sensitive` arg building, `:cs` upperdir validation, and error translations.

### Changed
- Prioritized drive letter `L:` first (`L:..Z:`, `E:..K:`, `D:`) in `DriveLetterService` and standardized CLI usage, GUI hints, and documentation examples on `L:`.
- Clarified GUI header language selector labels (`🌐 Auto (JA)` / `🌐 Auto (EN)` / `🌐 English` / `🌐 日本語`).

### Fixed
- Themed WPF `ComboBox` and `ComboBoxItem` dropdown popups in `Controls.xaml` so Dark mode no longer renders white text on a white popup background.
- Moved "Add Image..." and "New Image..." buttons to the left sidebar footer (`UniformGrid`) to prevent right-edge header clipping of the theme toggle button.

## [0.7.0] - 2026-11-22

### Changed
- Standardized the OSS repository layout and engineering documentation structure: organized `docs/` into lowercase kebab-case paths (`docs/README.md`, `docs/overview.md`, `docs/design/`, `docs/reports/`, `docs/guides/`, `docs/testing.md`, `docs/roadmap.md`) and extracted `ADR-001` through `ADR-013` into individual records under `docs/adr/`.
- Split `.github/workflows/ci.yml` into dedicated CI (`ci.yml`) and tag release (`.github/workflows/release.yml`) workflows.

### Added
- Added `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `AGENTS.md`, `.github/ISSUE_TEMPLATE/`, and `.github/PULL_REQUEST_TEMPLATE.md`.

## [0.6.0] - 2026-11-21

### Fixed
- Added WinFsp/ext4 target verification (`QueryDosDeviceW` / `GetVolumeInformationW`) in CLI `zeroext4 unmount` and `ZeroExt4Gui` residual-drive cleanup to prevent accidental detachment of non-ZeroExt4 volumes.
- Expanded `winfsp_callbacks` and `ZeroExt4.Gui.Tests` coverage with safe-unmount target validation tests.

## [0.5.0] - 2026-11-21

### Changed
- Upgraded bundled upstream `e2fsprogs` (`libext2fs`) from `1.47.2` to `1.47.4` (`scripts/update_e2fsprogs.ps1`), adding `fs_lock` serialization around shared `fs->icache` state and re-evaluating same-SSD NTFS / ZeroExt4 / WSL2 benchmarks.
- Excluded `zeroext4_bench.exe` from GitHub Release archives.
- Updated the multi-phase project roadmap covering safety guards, Overlay diff management, `fsck-lite`, and distribution.

### Added
- Added live English/Japanese localization (`i18n`) switching in `ZeroExt4Gui.exe` (`Strings.en.xaml`, `Strings.ja.xaml`, `LocalizationService`) and localization completeness tests.

## [0.4.0] - 2026-11-21

### Added
- Added GitHub Actions CI (`ci.yml`) with Visual Studio toolchain auto-discovery (`scripts/find_vs.ps1`) and automated GitHub Release packaging on `v*.*.*` tags.
- Added bilingual English `README.md` paired with Japanese `README.ja.md`.
- Added GPLv2 `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `.gitignore` for open-source release.

## [0.3.0] - 2026-11-21

### Added
- Added lockless thread-local Perfetto Chrome Trace Event profiling (`--trace`, `--trace-events`, `src/trace/perfetto_trace.c`) and quantitative bottleneck analysis report.
- Added AddressSanitizer (`ZEROEXT4_ENABLE_ASAN`), MSVC Debug CRT heap leak check (`ZEROEXT4_ENABLE_LEAK_CHECK`), `/analyze` static code analysis (`ZEROEXT4_ENABLE_ANALYZE`), orchestrated by `scripts/run_sanitizers.ps1`, plus concurrent RW and dentry-invalidation stress tests.

### Changed
- Completed Step 1–4 I/O and metadata performance optimizations: `FILE_FLAG_OVERLAPPED` + FLS events eliminating kernel `FileObject->Lock` serialization, `FileInfoTimeout = INFINITE` default on Read-Only mounts for NT Cache Manager (`CcCopyRead`) cooperation, 32-shard CLOCK `win32_io` cache, 16-shard `extent_cache` + open-handle extent caching, 16-shard positive/negative dentry cache, `FspGetDirInfoByName` fast lookup, and single-pass `ext4_vfs_open_stat` / `ext4_vfs_write_stat`.

## [0.2.0] - 2026-11-19

### Added
- Added desktop GUI mount manager `ZeroExt4Gui.exe` (`gui/ZeroExt4.Gui/`, .NET 10 WPF) and `ZeroExt4.Gui.Tests` with zero external NuGet dependencies.
- Added loosely coupled JSON / Named Event IPC modes (`--json`, `--json-events`, `--stop-event`, `--stats-interval`) on `zeroext4.exe` so the WPF GUI never links GPLv2 `libext2fs` directly.

## [0.1.0] - 2026-11-19

### Added
- Initial **ZeroExt4** (`zeroext4.exe`) release built on upstream `e2fsprogs` (`libext2fs`) + `WinFsp`.
- Implemented lockless Direct Extent I/O engine (`src/extent/`, enabled by default for Read-Only and `--overlay`), non-destructive NTFS Overlay VFS (`--overlay` with `.wh.__ext4_lower_id` lower-volume guard), Dirty Journal safety protection, and `UmFileContextIsUserContext2 = 1` per-open handle context tracking.
- Added Direct I/O benchmark tool (`tests/bench_direct_io.c`) and same-SSD NTFS / ZeroExt4 / WSL2 comparison suite (`scripts/bench_compare_ssd.ps1`).

[Unreleased]: https://github.com/totatake/ZeroExt4/compare/v0.10.1...HEAD
[0.10.1]: https://github.com/totatake/ZeroExt4/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/totatake/ZeroExt4/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/totatake/ZeroExt4/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/totatake/ZeroExt4/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/totatake/ZeroExt4/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/totatake/ZeroExt4/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/totatake/ZeroExt4/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/totatake/ZeroExt4/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/totatake/ZeroExt4/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/totatake/ZeroExt4/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/totatake/ZeroExt4/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/totatake/ZeroExt4/releases/tag/v0.1.0
