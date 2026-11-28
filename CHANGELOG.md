# Changelog

All notable changes to **ZeroExt4** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.11.0] - 2026-11-28

### Added
- **Phase 1 — Safety & Diagnostics**:
  - Extended `zeroext4 inspect` / `list --json` diagnostics with `partition_uuid`, readable feature-flag arrays (`feature_compat_names`, `feature_incompat_names`, `feature_ro_compat_names`), `last_mount_time`, `last_fsck_time`, `fsck_error_count`, `metadata_csum_enabled`, `sb_checksum_valid`, and `mount_advice` (`read_only_allowed`, `overlay_allowed`, `read_write_allowed`, `recommended_mode`, `reasons`), plus automatic RW suppression / RO preset fallback in `ZeroExt4Gui.exe` (`MountAdvice.ReadWriteAllowed == false`).
  - Added `zeroext4 doctor` CLI subcommand and GUI Diagnostics panel integration to check WinFsp runtime status/version, process elevation, target volume health, and `--overlay` upperdir compatibility in one command.
  - Upgraded `.wh.__ext4_lower_id` to v2 format (`v2;uuid=...;blocks=...;bsize=...;part_uuid=...;part_off=...;part_size=...;cs=...`) to detect cloned `.img`/VHDX volumes and layout mismatches while maintaining v1 backward compatibility; rejected non-empty unmarked upperdirs (`-ENOTEMPTY`), corrupted markers (`-EINVAL`), and unknown future marker versions (`-ENOTSUP`).
- **Phase 2 — Overlay Operations, Scalability & Namespace**:
  - Added `zeroext4 overlay <status|diff|validate|export|reset> --upper <dir>` CLI subcommands (`src/overlay/overlay_tools.c`)—including POSIX USTAR `.tar` export with `manifest.json`—along with preview, `.tar` export, and confirmed reset actions in `ZeroExt4Gui.exe`.
  - Replaced linear `O(N)` overlay inode/path maps with dynamically resizing bidirectional hash tables (`O(1)` lookup with shared-lock fast path) and deterministic hash-derived synthetic inodes (`0x80000000U+`) preserved across remounts and renames (`src/overlay/overlay_vfs.c`), plus pre-copyup tree sizing, NTFS free-space validation (`-ENOSPC`), and partial-copy rollback (`measure_lower_tree_copyup`).
  - Added WinFsp Reparse Point symlink pass-through (`IO_REPARSE_TAG_SYMLINK`, `ResolveReparsePoints`, `GetReparsePoint`, `SetReparsePoint`, `DeleteReparsePoint`) with POSIX-absolute-to-volume-relative translation, directory symlink post-processing, 16-hop loop guard, host-escape blocking, and overlay lower readlink/whiteout support (blocking symlink copy-up/creation/rename with `-ENOTSUP`).
  - Added Windows namespace safety guards (`src/common/win_namespace.c`) blocking creation, rename, and overlay copy-up of Windows reserved device names (`CON`, `NUL`, `AUX`, `PRN`, `COM1..9`, `LPT1..9` including extensions), forbidden characters, and trailing dots/spaces while keeping lower RO reads accessible, plus deterministic case-insensitive collision resolution (exact match -> lexicographical minimum -> lowest inode) with `stat_ci_collisions` tracking.
- **Phase 3 — ext4 Compatibility, Read-Only `fsck-lite` & Stateless RW Policy**:
  - Added explicit detection and RW guards for unsupported/restricted ext4 features (`s_journal_dev` external journal, `encrypt` / `fscrypt`, `verity`, `shared_blocks`, unknown `incompat`/`ro_compat` bits) across `superblock_probe`, `mount_advice`, `ext4_vfs_mount`, and `inspect`, plus `--ignore-csum-errors` for RO / `--overlay` rescue mounts (rejected under `--rw` with `-EINVAL`).
  - Added non-destructive 4-pass read-only ext4 consistency diagnostics (`zeroext4 fsck-lite`, `src/disk/fsck_lite.c`) and GUI integration (`ZeroExt4CoreClient.RunFsckLiteAsync`) checking superblocks, block group descriptors, bitmaps, extent trees / duplicate blocks, directory entries, orphan chains, and JBD2 headers without modifying the image/disk.
  - Expanded test coverage to 51 CTest functions and 12 GUI automated tests (including `test_ext4_format_boundaries_phase3`, `test_rw_standard_stateless_coherence`, `test_fsck_lite_clean_and_corruptions`, `test_overlay_tools_status_diff_validate_export_reset`, `test_winfsp_symlink_reparse_points_and_namespace_guards`, and `Phase3_FsckLiteArgsAndLiveIntegration`).

### Changed
- Formalized the stateless upstream `libext2fs` (`ext2fs_file_open` -> I/O -> `ext2fs_file_close`) reference policy for default `--rw` mounts (keeping custom batch optimizations strictly gated behind opt-in `--rw --rw-fast-io`), and fixed `inline_data` read clamping to `EXT2_I_SIZE` in `ext4_vfs_standard_read_fallback_locked()` (`src/vfs/op_read.c`).
- Re-measured all 4 I/O modes and same-NVMe-SSD 5-backend benchmarks on the current codebase (`2026-11-28`), clarified `--rw-fast-io` vs default modes in [`docs/reports/performance-evaluation.md`](docs/reports/performance-evaluation.md), and synchronized figures and version references to `v0.11.0` across `README.md`, `README.ja.md`, `docs/overview.md`, `docs/reports/ext4windows-comparison.md`, `docs/reports/perfetto-trace-analysis.md`, `docs/roadmap.md`, `docs/testing.md`, `docs/requirements.md`, `docs/architecture.md`, `docs/design/detailed-design.md`, and `SECURITY.md`.

## [0.10.2] - 2026-11-28

### Fixed
- Hardened `--rw-fast-io` crash/error consistency and guard symmetry: write physical data blocks before inserting new extent metadata in `src/vfs/op_write.c` (with partial-write block/inode-block-count rollback on error so exposed extents never point at stale disk data), symmetrized `EXT2_INODE_SIZE` / `EXT4_EXTENTS_FL` / `EXT4_INLINE_DATA_FL` / `EXT4_ENCRYPT_FL` guards across both `src/vfs/op_read.c` and `src/vfs/op_write.c`, and unconditionally invalidated the `win32_io` cache slot on write errors in `src/io/win32_io.c` to prevent stale dirty cache retention on partial I/O failure.

### Added
- Added configurable `ZEROEXT4_FSX_SEED` / `ZEROEXT4_FSX_OPS` overrides to `test_rw_fast_io_fsx_fuzzer` and a new 2 GiB / 4 GiB sparse-boundary & `EXT2_FEATURE_RO_COMPAT_LARGE_FILE` integration test (`test_rw_fast_io_large_file_4gb_sparse_boundary`) in `tests/test_vfs_operations.c` (40 CTest functions total).
- Extended `scripts/verify_e2fsck_wsl.ps1` with `-RandOps` / `-RandSeed` live Win32 `FileStream` randomized write/truncate/rename workload (`rand_fsx.bin`) verified against WSL2 `e2fsck -fn`, Linux kernel `mount -t ext4 -o loop,ro` SHA-256, and ZeroExt4 RO Direct Extent I/O readback.

## [0.10.1] - 2026-11-27

### Added
- Added opt-in `--rw-fast-io` mount option across VFS (`ext4_mount_config_t.enable_rw_fast_io`), CLI (`zeroext4 mount --rw --rw-fast-io`, rejected outside `--rw`), WPF GUI (`MainWindow.xaml` / `MainViewModel` / `ZeroExt4ArgBuilder` / `VolumePreset` / localized strings / `ArgBuilder` tests), WinFsp callback E2E test (`test_winfsp_rw_fast_io_e2e` including `ConstrainedIo` / `WriteToEndOfFile` / `SetFileSize` / `Overwrite` / `SetDelete` / RO Direct I/O readback), bidirectional WSL2 Linux kernel verification script (`scripts/verify_e2fsck_wsl.ps1`: real `mount -t ext4 -o loop` + `e2fsck -fn` + RO SHA-256 verification), WinFsp official test suite runner (`scripts/run_winfsp_tests.ps1`: 45/45 applicable `winfsp-tests-x64.exe --fuse-external` tests + post-test `e2fsck -fn` passing on both `--rw` and `--rw --rw-fast-io`), and unit tests (`test_rw_batched_extent_write_and_read_coherence`, `test_rw_extent_tree_split_reverse_and_fsck_integrity`, `test_rw_uninit_extent_fallback_and_truncate_stress`, `test_rw_fast_io_multileaf_hole_boundary_write_and_read`, `test_rw_fast_io_fsx_fuzzer`, and 10-thread `test_rw_fast_io_concurrent_rw_stress`; 39 CTest functions total).

### Fixed
- Fixed multi-leaf extent-tree (`eh_depth >= 1`) hole-boundary navigation in `--rw-fast-io` (`src/vfs/op_write.c`, `src/vfs/op_read.c`): replaced `EXT2_EXTENT_NEXT_LEAF` after `ext2fs_extent_goto()` (whose `EXT2_EXTENT_DOWN` leaves interior `visit_num == 0`) with explicit sibling/ancestor traversal (`find_next_leaf_after()`), preventing overlapping extent insertion on cross-leaf hole writes, preventing silent zero-reads across leaf-boundary holes, and unconditionally restoring handle level to `left_ext` on `EXT2_ET_EXTENT_NO_NEXT`.
- Fixed `FspRename_impl` directory-replacement semantics (`src/driver/winfsp_driver.c`): reject replacing a different existing directory when `ReplaceIfExists` is set with `STATUS_ACCESS_DENIED` (matching Windows/WinFsp `rename_test` semantics, with regression coverage in `test_winfsp_interface_e2e`), and hardened `FspWrite_impl` `ConstrainedIo` length clamping against addition overflow.

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

[Unreleased]: https://github.com/totatake/ZeroExt4/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/totatake/ZeroExt4/compare/v0.10.2...v0.11.0
[0.10.2]: https://github.com/totatake/ZeroExt4/compare/v0.10.1...v0.10.2
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
