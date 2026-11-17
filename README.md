# `libext2fs-win` (`ext4fsp`)

Windows 11 向けの **`libext2fs` (`e2fsprogs`) + `WinFsp`** ベース ext2 / ext3 / ext4 ユーザー空間ファイルシステムドライバおよび管理 CLI ツールです。

## 特徴

- **本家 `e2fsprogs` (`libext2fs`) 採用**: 64bit ブロック番号、ext4 エクステント (`EXT4_EXTENTS_FL`)、256B+ iノード（ナノ秒タイムスタンプ・`crtime`）、`metadata_csum` (CRC32C)、HTree ディレクトリに対応。
- **WinFsp による高速・安全なユーザー空間 FSD**: Windows の Cache Manager (`Cc`) / Memory Manager (`Mm`) 連携による高速シーケンシャル Read / `mmap` に対応しつつ、ドライバ署名不要・BSOD リスクゼロで動作。
- **Windows ネイティブ I/O マネージャ (`win32_io_manager`)**:
  - `CreateFileW` + `OVERLAPPED` によるスレッドセーフなオフセット指定 I/O
  - 4KB セクタ境界の自動 Read-Modify-Write (RMW)
  - GPT / MBR / EBR パーティション境界ガード（他パーティションへの誤書込を物理遮断）
  - 2 階層 `SRWLOCK`（データ Read 並列化＋メタデータ直列化）と LRU ブロックキャッシュ
- **Dirty Journal 安全保護**: マウント前に `EXT3_FEATURE_INCOMPAT_RECOVER` / `EXT2_ERROR_FS` / JBD2 スーパーブロック (`s_start != 0`) を検査し、未再生ジャーナルが残るボリュームへの不用意な RW マウントを自動抑止（または Read-Only 自動フォールバック）。
- **Zero-Patch `e2fsprogs` 追従構造**: `third_party/e2fsprogs` への Windows 固有ハックを排除し、`compat/` と `scripts/generate_e2fsprogs_headers.py` へ分離。

## ドキュメント

- [要件定義書](docs/requirements.md)
- [アーキテクチャ設計書・ADR (設計判断記録)](docs/architecture.md)
- [詳細設計書](docs/detailed_design.md)
- [テスト報告書・利用ガイド](docs/testing_and_usage.md)

## ビルドとテスト

Visual Studio 2022 BuildTools 環境で以下を実行します（ビルド時に WinFsp SDK の事前導入は不要です）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1
```

## WinFsp のインストール（ドライブマウント実行時に必須）

`ext4fsp.exe mount` を使用して Windows ドライブレター（例: `M:`）へマウントするには、事前に **WinFsp** ランタイムをインストールする必要があります（オフラインコマンド `list` / `inspect` / `mkfs` / `ls` / `cat` / `write` 等は WinFsp なしでも動作します）。

以下のいずれかの方法でインストールしてください：

- **winget を使用する場合（推奨）**:
  ```powershell
  winget install WinFsp.WinFsp
  ```
- **Chocolatey を使用する場合**:
  ```powershell
  choco install winfsp
  ```
- **公式インストーラ（`.msi`）を使用する場合**:
  1. [WinFsp 公式サイト](https://winfsp.dev/) または [GitHub Releases](https://github.com/winfsp/winfsp/releases) から最新の `winfsp-*.msi` をダウンロードして実行します。
  2. セットアップ種別は既定の **Core** のみで動作します（実行時に `C:\Program Files (x86)\WinFsp\bin\winfsp-x64.dll` を自動検出・動的ロードします）。

詳細なビルド手順・テスト結果・CLI リファレンスは [docs/testing_and_usage.md](docs/testing_and_usage.md) を参照してください。

## CLI 使用例

```powershell
# ディスク / パーティション一覧と状態検査
ext4fsp.exe list
ext4fsp.exe inspect test.img

# クリーンな ext4 イメージ作成・操作
ext4fsp.exe mkfs test.img --size-mb 64 --label MYEXT4
ext4fsp.exe mkdir test.img /data
ext4fsp.exe write test.img /data/hello.txt "Hello from libext2fs-win!"
ext4fsp.exe ls test.img /data
ext4fsp.exe cat test.img /data/hello.txt

# WinFsp で Windows ドライブへマウント (WinFsp ランタイム要)
ext4fsp.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --rw
```

## ライセンス

本プロジェクトは `e2fsprogs` (`libext2fs`) に基づくため **GNU General Public License v2.0 (GPLv2)** の下で提供されます。詳細は [LICENSE](LICENSE) を参照してください。
