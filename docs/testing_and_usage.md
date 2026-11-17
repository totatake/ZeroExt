# テスト報告書・利用ガイド (`libext2fs-win` / `ext4fsp`)

---

## 1. ビルド手順

### 必須要件（ビルド時）
- Visual Studio 2022（または BuildTools 2022：`cl.exe`, `cmake.exe`, `ninja.exe`, `ctest.exe`）
- Python 3.x（`%LOCALAPPDATA%\Python\bin\python.exe` を優先検出し、`ext2_err.et.in` と `ext2_types.h.in` から自動生成）
- ※ **WinFsp SDK はビルド時には不要**です（[winfsp.h](../third_party/winfsp/include/winfsp/winfsp.h) に互換 ABI 定義と `winfsp-x64.dll` 動的ローダーを内蔵しているため）。実ドライブレターへマウントする実行時のみ、WinFsp ランタイムのインストールが必要です。

### ワンショットビルド＆テスト実行

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1 -Clean
```

生成物（`build/` 配下）：
- `build/ext2fs.lib`: e2fsprogs (`lib/et` + `lib/ext2fs`) 静的ライブラリ
- `build/ext4fsp_core.lib`: `win32_io` / `disk` / `vfs` / `winfsp_driver` コア静的ライブラリ
- `build/ext4fsp.exe`: CLI ＆ WinFsp マウント実行ファイル
- `build/ext4fsp_tests.exe`: 単体・結合テスト実行ファイル

---

## 2. CLI リファレンス & WinFsp マウント手順

### 2.1 オフライン検査・イメージ操作

```powershell
# 物理ドライブ一覧またはイメージファイルのパーティション解析（管理者権限で物理ディスクアクセス可）
.\build\ext4fsp.exe list
.\build\ext4fsp.exe inspect <image_or_disk> [--partition <N>]

# ext4 イメージ新規作成（4KB ブロック・256B iノード・extents・64bit・metadata_csum 有効）
.\build\ext4fsp.exe mkfs test.img --size-mb 64 --label MYEXT4

# ファイル操作（イメージまたは物理パーティションに対して直接実行）
.\build\ext4fsp.exe mkdir test.img /data [--partition <N>]
.\build\ext4fsp.exe write test.img /data/hello.txt "Hello ext4!" [--partition <N>]
.\build\ext4fsp.exe write test.img /data/model.bin C:\path\to\local.bin --from-file [--partition <N>]
.\build\ext4fsp.exe ls    test.img /data [--partition <N>]
.\build\ext4fsp.exe stat  test.img /data/hello.txt [--partition <N>]
.\build\ext4fsp.exe cat   test.img /data/hello.txt [--partition <N>]
.\build\ext4fsp.exe mv    test.img /data/hello.txt /data/renamed.txt [--partition <N>]
.\build\ext4fsp.exe rm    test.img /data/renamed.txt [--partition <N>]
```

### 2.2 WinFsp のインストールと Windows ドライブレターへのマウント

> [!IMPORTANT]
> 実機のドライブレター（例: `M:`）としてマウントするには、事前に **WinFsp ランタイム** がインストールされている必要があります（`ext4fsp.exe` が実行時に `winfsp-x64.dll` を自動検出・ロードします）。
> 物理ディスク（`\\.\PhysicalDriveX`）を対象にする場合は、**管理者権限のターミナル** から実行してください。

#### WinFsp のインストール方法

以下のいずれかの方法でインストールできます（実行に必要なのは **Core** コンポーネントのみです）：

1. **winget でインストール（推奨）**
   ```powershell
   winget install WinFsp.WinFsp
   ```
2. **Chocolatey でインストール**
   ```powershell
   choco install winfsp
   ```
3. **公式 `.msi` インストーラで手動インストール**
   - [WinFsp 公式サイト](https://winfsp.dev/) または [GitHub Releases](https://github.com/winfsp/winfsp/releases) から最新の `winfsp-<version>.msi` をダウンロードして実行します。
   - コンポーネント選択画面では既定の **Core** のまま進めて完了します（再起動は通常不要です）。

インストール確認（`winfsp-x64.dll` およびレジストリキーの存在確認）：

```powershell
Test-Path "C:\Program Files (x86)\WinFsp\bin\winfsp-x64.dll"
Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\WinFsp" -Name InstallDir
```

#### マウント実行コマンド

```powershell
# Read-Only マウント（既定：最も安全）
.\build\ext4fsp.exe mount --source test.img --mountpoint M: --ro

# Read-Write マウント（Dirty Journal 検出時は自動拒否）
.\build\ext4fsp.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --rw

# オプション
#   --cache-blocks <N> : LRU ブロックキャッシュ数（既定 2048 ブロック = 8MB @ 4KB）
#   --force-dirty      : Dirty Journal 検出時でも強制的に RW オープン（非推奨・上級者向け）
#   --debug            : WinFsp デバッグログ有効化
```

アンマウントは実行中のコンソールで `Ctrl+C` を押下します（`ext2fs_flush` と I/O キャッシュフラッシュを実行したうえで安全に停止・解除します）。

プロセスが強制終了や異常終了してドライブレターだけが残ってしまった場合は、マウント時と同じ権限（物理ディスクの場合は管理者権限）で以下を実行してドライブ定義を解除できます：

```powershell
.\build\ext4fsp.exe unmount M:
```

> **管理者権限で実行した場合のドライブ表示について**
> 物理ディスクのマウントには管理者権限が必要です。昇格したプロセスが作るドライブレターは通常、昇格セッションからしか見えませんが、`ext4fsp` は昇格を検知すると Mount Manager 経由のグローバルドライブ (`\\.\X:`) として登録します。これにより、非昇格の Explorer からもドライブが見えます。Mount Manager での登録に失敗した場合は、デスクトップユーザーのセッションにもドライブを定義します（[ADR-009](architecture.md)）。指定したドライブレターがすでに使われている場合（カードリーダーなど）は `SetMountPoint` が `0xC0000022` などで失敗するので、空いているドライブレターを指定してください。

---

## 3. `e2fsprogs` アップストリーム更新手順

本プロジェクトでは `third_party/e2fsprogs/` に Windows 固有パッチを一切含めない Zero-Patch 分離構成を採用しています（`compat/msvc_compat.h` への `/FI` 強制インクルードと `compat/include/` に隔離）。

新しい `e2fsprogs` バージョンを展開したディレクトリ `<path>` を指定して以下を実行すると、ソース同期・ヘッダ自動生成・ビルド・`ctest` 検証までを一括実行できます：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\update_e2fsprogs.ps1 -UpstreamDir <path\to\e2fsprogs-x.y.z>
```

---

## 4. テスト仕様と実行結果レポート

### 4.1 自動テストスイート（`ctest` / `ext4fsp_tests.exe`）

| CTest スイート | テスト関数 | 主な検証内容 | 結果 |
| :--- | :--- | :--- | :--- |
| `win32_io` | `test_4kb_rmw_and_alignment` | 4KB セクタ設定時の 1024B 非アライメント Write (RMW) と周辺バイト無破損 | PASS |
| `win32_io` | `test_partition_boundary_guard` | パーティション `[offset, offset+size)` 境界外 Read/Write の拒否 (`EXT2_ET_LLSEEK_FAILED`) | PASS |
| `win32_io` | `test_read_only_and_lru_cache` | RO モード書込拒否 (`EXT2_ET_RO_FILSYS`)、LRU キャッシュヒット計測 | PASS |
| `partition_and_probe` | `test_gpt_disk_and_embedded_ext4` | 保護 MBR + GPT ヘッダ解析、UTF-16LE パーティション名、内蔵 ext4 解析 | PASS |
| `partition_and_probe` | `test_mbr_and_ebr_logical_partitions` | MBR + 拡張 EBR チェーン解析（論理パーティション 5 の ext4 検出） | PASS |
| `partition_and_probe` | `test_dirty_journal_and_jbd2_probe` | JBD2 inode 8 スーパーブロック（Big-Endian `s_start != 0`）および `EXT2_ERROR_FS` 検出 | PASS |
| `vfs_operations` | `test_vfs_crud_large_extent_and_persistence` | `mkfs`, `mkdir`, >3MB ファイル書込/読込, `readdir`, `rename`, Fast/Block symlink, `truncate`, `unlink`, 再マウント永続性 & CRC32C 検証 | PASS |
| `vfs_operations` | `test_fragmented_multi_level_extent_tree` | 断片化ファイルによるエクステントツリー深さ拡張 (`depth 0 -> 1`) の読み書き整合性 | PASS |
| `vfs_operations` | `test_concurrent_parallel_reads` | 8 スレッド並列 `ext4_vfs_read` (`SRWLOCK` Shared) のデータ整合性 | PASS |
| `vfs_operations` | `test_dirty_journal_safety_policy` | Dirty Journal 検出時の RW マウント拒否 (`-EROFS`) ＆ RO 自動フォールバック | PASS |
| `vfs_operations` | `test_multiblock_directory_expansion` | 120 ファイル作成による `ext2fs_expand_dir` 複数ブロックディレクトリ拡張・部分削除・再マウント検証 | PASS |
| `vfs_operations` | `test_gpt_partition_rw_isolation` | GPT Partition 2 への RW マウント書込時に Partition 1 の保護バイト (`0x5A`) が不変であること | PASS |
| `winfsp_callbacks` | `test_winfsp_interface_e2e` | `FSP_FILE_SYSTEM_INTERFACE` 全主要コールバック（`GetVolumeInfo`, `SetVolumeLabel`, `GetSecurityByName`, `Create`, `Open`, `Overwrite`, `Read`/`EOF`, `Write`, `Flush`, `SetBasicInfo`, `SetFileSize`, `CanDelete`, `SetDelete`, `Rename`, `ReadDirectory` Marker ページング, `Cleanup`, `Close`） | PASS |

### 4.2 実行ログ抜粋（クリーンビルド・Release・MSVC 19.44.35228 x64）

```text
Internal ctest changing into directory: D:/libext2fs-win/build
Test project D:/libext2fs-win/build
    Start 1: win32_io
1/4 Test #1: win32_io .........................   Passed    0.09 sec
    Start 2: partition_and_probe
2/4 Test #2: partition_and_probe ..............   Passed    0.10 sec
    Start 3: vfs_operations
3/4 Test #3: vfs_operations ...................   Passed    0.16 sec
    Start 4: winfsp_callbacks
4/4 Test #4: winfsp_callbacks .................   Passed    0.03 sec

100% tests passed, 0 tests failed out of 4
Total Test time (real) =   0.39 sec
```

`ext4fsp_tests.exe` 直接実行結果：`Summary: failures=0, assertions=28167`（全 13 テスト関数成功）。

### 4.3 CLI 実動作検証結果

16 MB ext4 イメージに対し `mkfs` → `inspect` → `mkdir` → `write` → `cat` → `mv` → `ls` → `rm` の一連操作を実行し、すべて終了コード 0 で正常完了することを確認しました（検証用一時イメージは削除済み）。
また、`third_party/e2fsprogs` 配下に `_MSC_VER` / `_WIN32` / `windows.h` 等の Windows 固有記述が 0 件であることも確認済みです。

> [!NOTE]
> 本環境には WinFsp カーネルドライバ（`winfsp-x64.dll` / `winfsp-x64.sys`）が未インストールのため、実際のドライブレター（`M:` 等）を OS 上へ出現させる結合確認は未実施です（`FSP_FILE_SYSTEM_INTERFACE` 全コールバックの挙動は `winfsp_callbacks` インプロセス結合テストにて検証済み）。実ドライブマウントを行う場合は WinFsp をインストールしてください。
