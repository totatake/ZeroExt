# Changelog

All notable changes to **ZeroExt** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.8.1] - 2026-11-25

### Added
- Added [`docs/guides/README.md`](docs/guides/README.md) hub and [`docs/guides/development.md`](docs/guides/development.md) to separate developer build, `e2fsprogs` upstream sync, benchmark, and Perfetto tracing procedures from end-user instructions.

### Changed
- Standardized H1 titles, `## N.` / `### N.M` section numbering, and 1:1 index link labels across `docs/` and root hubs (`docs/README.md`, `docs/adr/README.md`, `docs/design/README.md`, `docs/reports/README.md`).
- Migrated requirement IDs in `docs/requirements.md` (and `ADR-0010`) from `FR-*` / `NFR-*` to `REQ-F-001..011` / `REQ-NF-001..004`, reflecting `ZeroExtGui.exe`, `--case-sensitive`, and safe `unmount`.
- Streamlined `README.md` and `README.ja.md` with strict 1:1 section/link parity and synchronized benchmark figures in `README.md`, `README.ja.md`, and `docs/overview.md` to the `78cd744` (`e2fsprogs 1.47.4`) evaluation.
- Updated `docs/architecture.md` (added `ZeroExtGui.exe` IPC boundary and `src/trace/`), `docs/design/detailed-design.md` (32-shard CLOCK / 16-shard `extent_cache` / `cached_extents`), `docs/roadmap.md` (removed duplicated completed sections and updated `ADR-0013` anchor), and `docs/reports/perfetto-trace-analysis.md` (reframed as a completed optimization report).

## [0.8.0] - 2026-11-23

### Added
- Added `--case-sensitive` mount mode in CLI (`zeroext mount`) and `ZeroExtGui.exe` Advanced Options, enabling NTFS per-directory `FileCaseSensitiveInfo` on `--overlay` upperdirs and its subdirectories.
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
- Added WinFsp/ext4 target verification (`QueryDosDeviceW` / `GetVolumeInformationW`) in CLI `zeroext unmount` and `ZeroExtGui` residual-drive cleanup to prevent accidental detachment of non-ZeroExt volumes.
- Expanded `winfsp_callbacks` and `ZeroExt.Gui.Tests` coverage with safe-unmount target validation tests.

## [0.5.0] - 2026-11-21

### Changed
- Upgraded bundled upstream `e2fsprogs` (`libext2fs`) from `1.47.2` to `1.47.4` (`scripts/update_e2fsprogs.ps1`), adding `fs_lock` serialization around shared `fs->icache` state and re-evaluating same-SSD NTFS / ZeroExt / WSL2 benchmarks.
- Excluded `zeroext_bench.exe` from GitHub Release archives.
- Updated the multi-phase project roadmap covering safety guards, Overlay diff management, `fsck-lite`, and distribution.

### Added
- Added live English/Japanese localization (`i18n`) switching in `ZeroExtGui.exe` (`Strings.en.xaml`, `Strings.ja.xaml`, `LocalizationService`) and localization completeness tests.

## [0.4.0] - 2026-11-21

### Added
- Added GitHub Actions CI (`ci.yml`) with Visual Studio toolchain auto-discovery (`scripts/find_vs.ps1`) and automated GitHub Release packaging on `v*.*.*` tags.
- Added bilingual English `README.md` paired with Japanese `README.ja.md`.
- Added GPLv2 `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `.gitignore` for open-source release.

## [0.3.0] - 2026-11-21

### Added
- Added lockless thread-local Perfetto Chrome Trace Event profiling (`--trace`, `--trace-events`, `src/trace/perfetto_trace.c`) and quantitative bottleneck analysis report.
- Added AddressSanitizer (`ZEROEXT_ENABLE_ASAN`), MSVC Debug CRT heap leak check (`ZEROEXT_ENABLE_LEAK_CHECK`), `/analyze` static code analysis (`ZEROEXT_ENABLE_ANALYZE`), orchestrated by `scripts/run_sanitizers.ps1`, plus concurrent RW and dentry-invalidation stress tests.

### Changed
- Completed Step 1–4 I/O and metadata performance optimizations: `FILE_FLAG_OVERLAPPED` + FLS events eliminating kernel `FileObject->Lock` serialization, `FileInfoTimeout = INFINITE` default on Read-Only mounts for NT Cache Manager (`CcCopyRead`) cooperation, 32-shard CLOCK `win32_io` cache, 16-shard `extent_cache` + open-handle extent caching, 16-shard positive/negative dentry cache, `FspGetDirInfoByName` fast lookup, and single-pass `ext4_vfs_open_stat` / `ext4_vfs_write_stat`.

## [0.2.0] - 2026-11-19

### Added
- Added desktop GUI mount manager `ZeroExtGui.exe` (`gui/ZeroExt.Gui/`, .NET 10 WPF) and `ZeroExt.Gui.Tests` with zero external NuGet dependencies.
- Added loosely coupled JSON / Named Event IPC modes (`--json`, `--json-events`, `--stop-event`, `--stats-interval`) on `zeroext.exe` so the WPF GUI never links GPLv2 `libext2fs` directly.

## [0.1.0] - 2026-11-19

### Added
- Initial **ZeroExt** (`zeroext.exe`) release built on upstream `e2fsprogs` (`libext2fs`) + `WinFsp`.
- Implemented lockless Direct Extent I/O engine (`src/extent/`, enabled by default for Read-Only and `--overlay`), non-destructive NTFS Overlay VFS (`--overlay` with `.wh.__ext4_lower_id` lower-volume guard), Dirty Journal safety protection, and `UmFileContextIsUserContext2 = 1` per-open handle context tracking.
- Added Direct I/O benchmark tool (`tests/bench_direct_io.c`) and same-SSD NTFS / ZeroExt / WSL2 comparison suite (`scripts/bench_compare_ssd.ps1`).

[Unreleased]: https://github.com/totatake/ZeroExt/compare/v0.8.1...HEAD
[0.8.1]: https://github.com/totatake/ZeroExt/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/totatake/ZeroExt/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/totatake/ZeroExt/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/totatake/ZeroExt/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/totatake/ZeroExt/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/totatake/ZeroExt/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/totatake/ZeroExt/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/totatake/ZeroExt/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/totatake/ZeroExt/releases/tag/v0.1.0
