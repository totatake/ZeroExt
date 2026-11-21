# `ZeroExt` (`zeroext`)

**Language:** English | [日本語](README.ja.md)

A high-performance **ext2 / ext3 / ext4** user-space filesystem driver, CLI tool (`zeroext.exe`), and desktop GUI mount manager (`ZeroExtGui.exe`) for Windows 11 / 10, built on upstream **`libext2fs` (`e2fsprogs`) + `WinFsp`**.

> **Why ZeroExt?** It is designed to share large AI models (GGUF / Safetensors, etc.) and development datasets stored on Linux partitions **directly with Windows at Native NTFS-class speed—without copying files, without risking ext4 corruption, and with zero BSOD risk** in user space.

## Comparison & Benchmark Highlights

| Approach | Mount Style | Warm Read (`1MB` / `4KB Rnd`) | Cold NVMe (`1MB`) | Modern ext4 | BSOD Risk | Non-Destructive Write |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`ZeroExt` (Default RO / DIO)** | **Drive (`M:` etc.)** | **`10,524` / `1,017 MB/s`**<br><sub>(`1.01x–1.04x` vs Native NTFS)</sub> | **`1,551 MB/s`** | **Yes (`libext2fs`)** | **Zero** | **Yes (`--overlay`)** |
| **WSL2 (`wsl --mount` + 9P)** | UNC (`\\wsl.localhost`) | `198` / `17.6 MB/s`<br><sub>(`1/53–1/69` of `ZeroExt`)</sub> | `168 MB/s` | Yes | Low (VM req.) | No |
| **Legacy Kernel FSD (`Ext2Fsd`, etc.)** | Drive | Fast | Fast | Partial / Legacy | **High** | No |
| **Commercial Drivers / Viewers** | Drive / Viewer | Fast / Copy req. | Fast / Copy req. | Yes / Partial | Medium / Zero | No |

### How It Achieves This Speed (2-Tier I/O Bypass)

```mermaid
flowchart LR
    App["Windows App / mmap (LM Studio / llama.cpp / etc.)"] --> Sys["winfsp-x64.sys (MS-Signed FSD)"]
    Sys -->|"Warm / RO FileInfoTimeout=INFINITE"| Hot["NT Cache Manager (CcCopyRead)\nUser-space bypass: 10.5 GB/s / 260K IOPS"]
    Sys -->|"Cold / Unbuffered"| Cold["Handle Extent Binary Search O(log N)\n+ FILE_FLAG_OVERLAPPED Parallel Direct Read: 1.55 GB/s"]
    App -.->|"Compare: WSL2 \\wsl.localhost"| Slow["9P / HVSocket VM Boundary RPC Roundtrips\n198 MB/s (4KB Rnd 17.6 MB/s)"]
```

- For architectural novelty, sequence diagrams, and full benchmark details, see **[Why ZeroExt: Architecture Comparison & Use Cases](docs/why_zeroext.md)** and **[Performance Evaluation Report](docs/performance_evaluation.md)** (Japanese docs).

## Key Use Cases

- **Dual-Boot Large AI Model Sharing**: `mmap` and load 10–100 GB GGUF / Safetensors models directly from your Linux ext4 partition into Windows LM Studio / `llama.cpp` / ComfyUI with zero duplication (pair with `--overlay` to isolate Windows-side caches/configs without touching ext4).
- **ML Datasets / Video & 3D Asset Sharing**: Traverse large directories smoothly via a 16-shard positive/negative dentry cache and `FspGetDirInfoByName`.
- **External Linux NVMe / microSD Recovery**: Inspect JBD2 dirty journals and mount read-only to safely recover data from Raspberry Pi / Jetson / Steam Deck / Linux server drives.
- **Non-Destructive Embedded / VM Image (`.img`) Inspection**: Test modifications in an NTFS `--overlay` while keeping the base `.img` untouched (offline CLI/GUI browsing also works without WinFsp).
- **Fast Access to Stopped WSL2 (`ext4.vhdx`) Images**: Bypass `\\wsl.localhost` (`17.6 MB/s`) and access files up to 57x+ faster on random reads from native Windows tools.

See [docs/why_zeroext.md](docs/why_zeroext.md#3-有効に利用できる実践ユースケース) for concrete command examples and caveats.

## Features

- **Upstream `e2fsprogs` (`libext2fs`)**: Full support for 64-bit block numbers, ext4 extents (`EXT4_EXTENTS_FL`), 256B+ inodes (nanosecond timestamps, `crtime`), `metadata_csum` (CRC32C), and HTree directories.
- **2-Tier High-Speed WinFsp I/O Pipeline**: Read-only mounts default to `FileInfoTimeout = INFINITE` to cooperate directly with the Windows NT Cache Manager (`CcCopyRead`) and Memory Manager (`Mm`) (configurable via `--file-info-timeout`). Metadata paths are accelerated with `PassQueryDirectoryFileName = 1`, `FspGetDirInfoByName`, early `Marker` skipping, and single-pass Open/Write Stat.
- **Direct Extent I/O Engine (Default ON for RO / Overlay; disable with `--no-direct-io`)**: Caches file extent maps on open file handles (`ext4_file_handle_t`) and a 16-shard shared-lock `extent_cache`, performing `O(log N)` binary search and lockless physical-sector reads (`win32_io_pread_direct`). Automatically disabled in `--rw` mode for consistency.
- **Non-Destructive NTFS Overlay Mount (`--overlay`)**: Keeps the lower ext4 volume strictly read-only while transparently redirecting copy-up writes, new files, and whiteout deletions to a Windows NTFS directory (with `.wh.__ext4_lower_id` verification to prevent mismatched lower volumes).
- **Native Windows Async I/O Manager (`win32_io_manager`)**:
  - True parallel async I/O via `CreateFileW(FILE_FLAG_OVERLAPPED)`, `FILE_SKIP_SET_EVENT_ON_HANDLE`, and FLS event reuse (eliminating `FileObject->Lock` serialization)
  - Automatic 4KB sector-boundary Read-Modify-Write (RMW) and GPT / MBR / EBR partition boundary guards
  - 32-shard (64B-aligned, `SRWLOCK` shared-read, CLOCK in-place eviction) block cache + 16-shard (epoch-based bulk invalidation) positive/negative dentry cache
- **Dirty Journal Safety Protection**: Inspects `EXT3_FEATURE_INCOMPAT_RECOVER`, `EXT2_ERROR_FS`, and JBD2 superblock (`s_start != 0`) before mounting, preventing unsafe RW mounts on unrecovered journals (or automatically falling back to Read-Only with `--ro-fallback`).
- **Perfetto Performance Tracing (`--trace`)**: Zero-overhead single-branch fast path when disabled; when `--trace <output.json>` is specified, records WinFsp callbacks, `SRWLOCK` wait/hold durations, Extent/LRU cache hits, Direct I/O, `ReadFile`/`WriteFile`, and Overlay Copy-up/Whiteout spans into lockless thread-local ring buffers (`QueryPerformanceCounter` precision) for visualization in [Perfetto UI](https://ui.perfetto.dev/).
- **Zero-Patch `e2fsprogs` Integration**: Keeps `third_party/e2fsprogs` free of Windows-specific patches by isolating compatibility shims in `compat/` and `scripts/generate_e2fsprogs_headers.py`.

## Documentation

*(Most detailed design and evaluation documents are currently written in Japanese.)*

- **[Why ZeroExt: Architecture Comparison & Use Cases (`why_zeroext.md`)](docs/why_zeroext.md)**
- [Requirements Specification](docs/requirements.md)
- [Architecture & ADRs](docs/architecture.md)
- [Detailed Design](docs/detailed_design.md)
- [Testing Report & Usage Guide](docs/testing_and_usage.md)
- [Performance Evaluation Report (`--direct-io` / NTFS / WSL2)](docs/performance_evaluation.md)
- [Perfetto Trace Quantitative Analysis & Optimization Plan](docs/perfetto_trace_analysis.md)
- [Future Roadmap](docs/future_roadmap.md)

## Building & Testing

Run the following on Windows with Visual Studio 2022 (BuildTools / Community / Professional / Enterprise) and Python 3.x (no WinFsp SDK installation is required at build time; if `.NET 10 SDK` is installed, `ZeroExtGui.exe` and `ZeroExt.Gui.Tests` are built and tested automatically):

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1
```

To build and test the GUI only:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_gui.ps1
```

To run AddressSanitizer, MSVC Debug CRT heap leak detection, and `/analyze` static analysis:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1
```

After building, launch the GUI mount manager from `.\build\gui\ZeroExtGui.exe`.

## GUI Application (`ZeroExtGui.exe`)

- **Disk / Image Discovery & Inspection**: Displays physical disks (with one-click "Restart as Administrator" when non-elevated) and `.img` files (drag-and-drop supported), including GPT / MBR / EBR partitions, labels, UUIDs, usage, and JBD2 journal status (`CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`).
- **Three Mount Modes**:
  - **Read-Only (Recommended / Default)**: Fast and safe reading with Direct I/O enabled.
  - **Overlay (Non-Destructive RW)**: Keeps the lower ext4 volume read-only while saving changes to a designated NTFS upper directory (with `.wh.__ext4_lower_id` guard).
  - **Read-Write**: Supports automatic read-only fallback (`--ro-fallback`) when a dirty journal is detected.
- **Live Stats & System Tray**: Real-time read/write throughput and cumulative transfer counters, "Open in Explorer", safe one-click unmount, system tray integration, and live CLI command preview.
- **Offline Features Without WinFsp**: Browse ext2/3/4 image folders/files (`File Browser` tab), create new `.img` files (`mkfs`), and view WinFsp setup guidance even without WinFsp installed.
- **Loosely Coupled Architecture**: Does not link GPLv2 `libext2fs` / `zeroext_core` directly; controls `zeroext.exe` as a subprocess via JSON / Named Event IPC.

## Installing WinFsp (Required for Drive Mounting)

To mount a volume to a Windows drive letter (e.g. `M:`) using `zeroext.exe mount`, install the **WinFsp** runtime first (offline commands such as `list` / `inspect` / `mkfs` / `ls` / `cat` / `write` work without WinFsp):

- **Using winget (Recommended)**:
  ```powershell
  winget install WinFsp.WinFsp
  ```
- **Using Chocolatey**:
  ```powershell
  choco install winfsp
  ```
- **Using the Official Installer (`.msi`)**:
  1. Download the latest `winfsp-*.msi` from [winfsp.dev](https://winfsp.dev/) or [GitHub Releases](https://github.com/winfsp/winfsp/releases).
  2. The default **Core** installation is sufficient (`zeroext.exe` dynamically discovers and loads `C:\Program Files (x86)\WinFsp\bin\winfsp-x64.dll` at runtime).

For full CLI reference and build/test details, see [docs/testing_and_usage.md](docs/testing_and_usage.md).

## CLI Examples

```powershell
# List physical disks / partitions and inspect an image
zeroext.exe list
zeroext.exe inspect test.img

# Create and manipulate a clean ext4 image
zeroext.exe mkfs test.img --size-mb 64 --label MYEXT4
zeroext.exe mkdir test.img /data
zeroext.exe write test.img /data/hello.txt "Hello from ZeroExt!"
zeroext.exe ls test.img /data
zeroext.exe cat test.img /data/hello.txt
zeroext.exe extents test.img /data/hello.txt

# Mount to a Windows drive letter via WinFsp (requires WinFsp runtime; defaults to RO + Direct Extent I/O ON)
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M:

# Explicitly disable Direct I/O and mount via standard LRU cache
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --no-direct-io

# Read-Write mount (explicit --rw; Direct I/O is automatically disabled)
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --rw

# Overlay mount: keep ext4 Read-Only + Direct I/O ON, and redirect writes to an NTFS folder
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --overlay D:\ext4_upper
```

## License

This project is licensed under the **GNU General Public License v2.0 (GPLv2)** due to linking with `e2fsprogs` (`libext2fs`). See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
