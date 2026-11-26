# アーキテクチャ設計書

## 1. システムアーキテクチャ概要

本システムは、カーネルプロキシとしての **WinFsp (`winfsp-x64.sys` / `winfsp-x64.dll`)**、疎結合デスクトップ GUI（`gui/ZeroExt4.Gui/` / `ZeroExt4Gui.exe`）、WinFsp コールバックブリッジ層（`src/driver/`）、NTFS Overlay ルーティング層（`src/overlay/`）、モジュラー VFS 操作層（`src/vfs/op_*.c`）、エクステント直結 I/O エンジン（`src/extent/`）、ディスク・パーティション・ジャーナル検査層（`src/disk/`）、組み込み Perfetto トレーサ（`src/trace/`）、本家 `e2fsprogs` (`third_party/e2fsprogs/` + `compat/`)、および Windows ネイティブ I/O マネージャ（`src/io/win32_io.c`）で構成される。

### 1.1 コンポーネント構成図

```mermaid
flowchart TD
    subgraph UserApps["Windows アプリケーション層 (非GPLv2 / IPC 境界)"]
        Exp["Windows Explorer / Win32 Apps / mmap (GGUF 等)"]
        GuiApp["ZeroExt4Gui.exe (.NET 10 WPF)\n--json / --json-events / --stop-event 経由で疎結合制御"]
        CliUser["管理者ターミナル (CLI)"]
    end

    subgraph Kernel["Windows カーネル空間"]
        WinFspSys["winfsp-x64.sys\n(MS署名済 FSD + Cc/Mm キャッシュ連携)"]
    end

    subgraph DriverExe["ZeroExt4 / zeroext4.exe (ユーザー空間・GPLv2)"]
        CLI["src/cli/main.c\n(list / inspect / extents / mount / unmount 等)"]
        WinFspDrv["src/driver/winfsp_driver.c\n(FSP_FILE_SYSTEM_INTERFACE + UACセッション対応)"]
        Overlay["src/overlay/overlay_vfs.c\n(--overlay 時: Upper NTFS + Copy-up + Whiteout)"]
        VFS["src/vfs/op_*.c\n(モジュラーVFS + 2階層SRWLOCK + Dentryキャッシュ)"]
        Extent["src/extent/extent_map.c\n(エクステント抽出 / 16シャードキャッシュ + 二分探索Read)"]
        Disk["src/disk/\n(partition_table / superblock_probe / disk_scanner)"]
        Trace["src/trace/perfetto_trace.c\n(--trace 時: ロックレス TLS リングバッファ)"]
        LibExt2["third_party/e2fsprogs/lib/ext2fs\n(64bit / Extents / CRC32C / 256B Inode)"]
        Compat["compat/ (msvc_compat.h + include/)\n+ scripts/generate_e2fsprogs_headers.py"]
        Win32IO["src/io/win32_io.c\n(OVERLAPPED + 直読API + 4KB RMW + 32シャードCLOCK + 境界ガード)"]
    end

    subgraph Storage["ストレージデバイス / Upperdir"]
        Phys["\\\\.\\PhysicalDriveX (GPT/MBR) / .img (Lower)"]
        NtfsUpper["Windows NTFS Upperdir (--overlay 指定時)"]
    end

    Exp --> WinFspSys
    WinFspSys <-->|"FSP_FILE_SYSTEM_INTERFACE"| WinFspDrv
    GuiApp -.->|"サブプロセス起動 / JSON IPC\n(libext2fs 直接リンク禁止)"| CLI
    CliUser --> CLI
    CLI --> Disk
    CLI --> VFS
    CLI --> WinFspDrv
    WinFspDrv --> VFS
    VFS -->|"--overlay 時"| Overlay
    Overlay -->|"書込 / Upper参照 / Copy-up"| NtfsUpper
    Overlay -->|"Lower 参照 (RO)"| Extent
    VFS -->|"通常Read (ハンドル/キャッシュヒット時ロックレス)"| Extent
    VFS --> Disk
    VFS --> LibExt2
    Compat -.->|"CMake /FI 非侵襲シム"| LibExt2
    Extent -->|"win32_io_pread_direct"| Win32IO
    LibExt2 -->|"io_manager"| Win32IO
    Disk --> Win32IO
    WinFspDrv -.->|"スパン・カウンタ記録"| Trace
    VFS -.->|"ロック・Dentry記録"| Trace
    Win32IO -.->|"I/O・キャッシュ記録"| Trace
    Win32IO -->|"CreateFileW / OVERLAPPED"| Phys
```

### 1.2 マウント ＆ I/O シーケンス

```mermaid
sequenceDiagram
    actor User as ユーザー
    participant CLI as zeroext4.exe (CLI / Driver)
    participant Disk as partition_table & superblock_probe
    participant VFS as ext4_vfs / overlay_vfs
    participant Ext as extent_map (LRUキャッシュ + 二分探索)
    participant IO as win32_io_manager
    participant Dev as PhysicalDrive / .img

    User->>CLI: zeroext4 mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint L: [--overlay C:\upper]
    CLI->>Disk: パーティション位置解決 (GPT/MBR/EBR) & ext4_probe_partition
    Disk-->>CLI: OffsetBytes, SizeBytes, Journal状態
    CLI->>VFS: ext4_vfs_mount(cfg) (Overlay時はLowerを厳密ROオープン)
    VFS-->>CLI: ext4_vfs_ctx_t 確立
    CLI->>CLI: WinFsp StartDispatcher & ドライブ/フォルダマウント

    User->>CLI: ファイル Read (例: llama.cpp の mmap ページイン)
    alt Overlay で Upper (NTFS) に存在
        VFS-->>User: Upper NTFS ファイルから直接 ReadFile
    else Lower (ext4) のファイルを Direct Extent I/O で読み取り
        Ext->>Ext: extent_cache 参照 (ヒット時は ctx->rwlock 不要)
        opt 初回キャッシュミス時のみ
            Ext->>IO: ext2fs_extent_get / block_iterate3 / inline_data_get 抽出
        end
        Ext->>Ext: O(log N) 二分探索で論理オフセット → 物理オフセット変換
        Ext->>IO: win32_io_pread_direct (cache_lock バイパス)
        IO->>Dev: ReadFile(OVERLAPPED) 連続ブロック一括直読
        IO-->>User: データ返却 (スパース領域はメモリ上でゼロ埋め)
    end
```

---

## 2. `e2fsprogs` バージョンアップ追従アーキテクチャ

上流の `e2fsprogs` 更新時に手作業のマージコンフリクトなく追従するため、以下の **Zero-Patch 分離構成** を採用する（技術背景は [ADR-0005](adr/0005-zero-patch-e2fsprogs-compat-layer.md)、実装仕様は [詳細設計書 §2](design/detailed-design.md#2-e2fsprogs-互換レイヤー詳細-compat--scripts) を参照）。

1. **ソースツリーへの手動パッチ禁止**: `third_party/e2fsprogs/` 配下へ手書きの `#ifdef _MSC_VER` / `#ifdef _WIN32` 改変を加えず、GCC/POSIX 互換シムはすべて `compat/` へ隔離する。
2. **CMake `/FI` の `PRIVATE` / `INTERFACE` 分離**: `ext2fs` ターゲットには `compat/msvc_compat.h`（警告抑制含む）と `compat/include/` を `PRIVATE` で適用し、呼び出し側ターゲットには最小公開シム `compat/msvc_public_compat.h` のみを `INTERFACE` で伝播させる。
3. **ビルド時ヘッダ・テーブル自動生成**: `scripts/generate_e2fsprogs_headers.py` が `ext2_err.h`, `ext2_err.c`, `ext2_types.h`, `crc32c_table.h` を CMake configure/build 時に生成する。
4. **I/O 抽象化境界の徹底**: OS 依存の `unix_io.c` 等を除外し、公開 ABI である `struct struct_io_manager` を実装した `src/io/win32_io.c` を注入する。
5. **再現可能な自動更新フロー**: MSVC C11 構文制約に対する最小限の上流調整も含め [開発・ビルド・保守ガイド §2](guides/development.md#2-e2fsprogs-アップストリーム更新手順)（`scripts/update_e2fsprogs.ps1`）で再現可能に自動化する。

---

## 3. 重要な設計判断 (ADR: Architecture Decision Records)

主要な技術選定・安全境界・並行制御・キャッシュ／I/O 方針の判断記録は [`docs/adr/`](adr/README.md) に個別の ADR として管理している。

| ADR | 決定内容 | 主な対象コンポーネント | 詳細 |
| :--- | :--- | :--- | :--- |
| `ADR-0001` | 純粋 `.sys` ではなく `WinFsp + ユーザー空間 libext2fs` を採用 | `src/driver/` | [0001-winfsp-and-libext2fs-userspace-driver.md](adr/0001-winfsp-and-libext2fs-userspace-driver.md) |
| `ADR-0002` | `LinuxRoDrive` 等と分離した独立 C/CMake プロジェクト（GPLv2 境界・GUI IPC 疎結合） | `CMakeLists.txt`, `gui/` | [0002-standalone-project-boundary.md](adr/0002-standalone-project-boundary.md) |
| `ADR-0003` | `lwext4` ではなく本家 `e2fsprogs` (`libext2fs`) を採用 | `third_party/e2fsprogs/` | [0003-upstream-libext2fs-over-lwext4.md](adr/0003-upstream-libext2fs-over-lwext4.md) |
| `ADR-0004` | 操作別モジュラー VFS × 64bit API × WinFsp ネイティブ接続 | `src/vfs/`, `src/driver/` | [0004-modular-vfs-64bit-api-and-native-winfsp.md](adr/0004-modular-vfs-64bit-api-and-native-winfsp.md) |
| `ADR-0005` | `compat/` + ヘッダ生成 + 再現スクリプトによる Zero-Patch 互換レイヤー | `compat/`, `scripts/` | [0005-zero-patch-e2fsprogs-compat-layer.md](adr/0005-zero-patch-e2fsprogs-compat-layer.md) |
| `ADR-0006` | `CreateFileW` + `OVERLAPPED` + 4KB RMW + 境界ガード付き `win32_io_manager` | `src/io/win32_io.c` | [0006-custom-win32-io-manager.md](adr/0006-custom-win32-io-manager.md) |
| `ADR-0007` | 階層化 `SRWLOCK`（Shared Read / Exclusive Write）と単一方向ロック順序固定 | `src/vfs/`, `src/io/` | [0007-two-tier-srwlock-concurrency.md](adr/0007-two-tier-srwlock-concurrency.md) |
| `ADR-0008` | マウント前 JBD2 / エラー検査による Dirty 時 RW 拒否と RO フォールバック | `src/disk/`, `src/vfs/op_init.c` | [0008-dirty-journal-safety-guard.md](adr/0008-dirty-journal-safety-guard.md) |
| `ADR-0009` | 管理者権限マウント時の `\\.\X:` 優先登録＋シェルセッション偽装補完 | `src/driver/winfsp_driver.c` | [0009-elevated-drive-letter-visibility.md](adr/0009-elevated-drive-letter-visibility.md) |
| `ADR-0010` | 自己完結 WinFsp ABI 構造体定義とコンパイル時 `FSP_ABI_ASSERT` | `third_party/winfsp/` | [0010-winfsp-abi-layout-assertions.md](adr/0010-winfsp-abi-layout-assertions.md) |
| `ADR-0011` | `UmFileContextIsUserContext2 = 1` による 1 オープン＝1 クローズ保証 | `src/driver/`, `src/vfs/ext4_vfs.h` | [0011-handle-context-via-user-context2.md](adr/0011-handle-context-via-user-context2.md) |
| `ADR-0012` | Lower RO 限定（RO / `--overlay` 既定 ON・`--rw` 自動無効）Direct Extent I/O | `src/extent/`, `src/vfs/op_read.c` | [0012-lockless-direct-extent-io-ro-overlay-default.md](adr/0012-lockless-direct-extent-io-ro-overlay-default.md) |
| `ADR-0013` | NTFS Upperdir Overlay と `.wh.__ext4_lower_id` Lower-ID 照合 | `src/overlay/` | [0013-ntfs-overlay-mount-and-lower-id-guard.md](adr/0013-ntfs-overlay-mount-and-lower-id-guard.md) |

