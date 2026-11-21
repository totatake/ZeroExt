# `ZeroExt` (`zeroext`)

Windows 11 / 10 向けの **`libext2fs` (`e2fsprogs`) + `WinFsp`** ベース高速 ext2 / ext3 / ext4 ユーザー空間ファイルシステムドライバ、管理 CLI ツール（`zeroext.exe`）、およびデスクトップ GUI マウントマネージャー（`ZeroExtGui.exe`）です。

## 特徴

- **本家 `e2fsprogs` (`libext2fs`) 採用**: 64bit ブロック番号、ext4 エクステント (`EXT4_EXTENTS_FL`)、256B+ iノード（ナノ秒タイムスタンプ・`crtime`）、`metadata_csum` (CRC32C)、HTree ディレクトリに対応。
- **WinFsp による高速・安全なユーザー空間 FSD**: Windows の Cache Manager (`Cc`) / Memory Manager (`Mm`) 連携による高速シーケンシャル Read / `mmap` に対応しつつ、ドライバ署名不要・BSOD リスクゼロで動作。
- **ダイレクト・エクステント I/O エンジン（RO / Overlay 時既定有効、`--no-direct-io` で無効化可）**: 既定の Read-Only マウントおよび `--overlay` マウントでは、初回アクセス時にエクステントマップ（多段ツリー・間接ブロック・インラインデータ対応）をキャッシュし、以降の `Read` を `O(log N)` 二分探索＋ロックレス物理セクタ直読 (`win32_io_pread_direct`) で高速化します（GGUF モデル等の大容量 `mmap` 読み込みに最適。`--rw` モードでは整合性維持のため自動的に無効化されます）。
- **非破壊 NTFS Overlay マウント (`--overlay`)**: Lower 側の ext4 を厳密 Read-Only に保ったまま NTFS ディレクトリを上層に重ね合わせ、Copy-up 書込・新規作成・ホワイトアウト削除を透過的に実現（`.wh.__ext4_lower_id` による別 Lower 誤流用ガード付き）。
- **Windows ネイティブ I/O マネージャ (`win32_io_manager`)**:
  - `CreateFileW` + `OVERLAPPED` によるスレッドセーフなオフセット指定 I/O
  - 4KB セクタ境界の自動 Read-Modify-Write (RMW)
  - GPT / MBR / EBR パーティション境界ガード（他パーティションへの誤書込を物理遮断）
  - 2 階層 `SRWLOCK`（データ Read 並列化＋メタデータ直列化）と LRU ブロックキャッシュ
- **Dirty Journal 安全保護**: マウント前に `EXT3_FEATURE_INCOMPAT_RECOVER` / `EXT2_ERROR_FS` / JBD2 スーパーブロック (`s_start != 0`) を検査し、未再生ジャーナルが残るボリュームへの不用意な RW マウントを自動抑止（または Read-Only 自動フォールバック）。
- **Perfetto 性能トレース出力 (`--trace`)**: 通常時は単一分岐によるゼロオーバーヘッドを維持しつつ、`--trace <output.json>` 指定時のみスレッドローカルリングバッファ（ロックレス記録・`QueryPerformanceCounter` 精度）へ WinFsp コールバック、`SRWLOCK` 待ち／保持時間、Extent／LRU キャッシュヒット、Direct I/O、`ReadFile`/`WriteFile`、Overlay Copy-up/Whiteout を記録し、[Perfetto UI](https://ui.perfetto.dev/) でタイムライン可視化が可能。
- **Zero-Patch `e2fsprogs` 追従構造**: `third_party/e2fsprogs` への Windows 固有ハックを排除し、`compat/` と `scripts/generate_e2fsprogs_headers.py` へ分離。

## ドキュメント

- [要件定義書](docs/requirements.md)
- [アーキテクチャ設計書・ADR (設計判断記録)](docs/architecture.md)
- [詳細設計書](docs/detailed_design.md)
- [テスト報告書・利用ガイド](docs/testing_and_usage.md)
- [性能評価報告書 (`--direct-io` / NTFS / WSL2 比較)](docs/performance_evaluation.md)
- [Perfetto トレース定量解析・性能改善計画](docs/perfetto_trace_analysis.md)
- [今後の構想・ロードマップ](docs/future_roadmap.md)

## ビルドとテスト

Visual Studio 2022 BuildTools 環境で以下を実行します（ビルド時に WinFsp SDK の事前導入は不要です。`.NET 10 SDK` が導入されている環境では GUI アプリ `ZeroExtGui.exe` も自動ビルド・テストされます）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1
```

GUI のみをビルド・テストする場合は以下を実行します：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_gui.ps1
```

ビルド完了後は `.\build\gui\ZeroExtGui.exe` から GUI マウントマネージャーを起動できます。

## GUI アプリケーション (`ZeroExtGui.exe`)

- **ディスク／イメージ自動一覧 & 状態検査**: 物理ディスク（非管理者時は 1 クリックで「管理者権限で再起動」）および `.img` ファイル（ドラッグ＆ドロップ対応）の GPT / MBR / EBR パーティション・ラベル・UUID・使用率・JBD2 ジャーナル状態（`CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`）を可視化。
- **3 つのマウントモード選択**:
  - **Read-Only（推奨・既定）**: Direct I/O 有効で高速・安全に読取。
  - **Overlay（非破壊 RW）**: Lower ext4 を RO 保護し、変更・新規作成だけ指定の NTFS Upperdir へ保存（`.wh.__ext4_lower_id` の事前検証付き）。
  - **Read-Write**: Dirty 状態検出時の自動 RO フォールバック設定（`--ro-fallback`）に対応。
- **リアルタイム統計・システムトレイ常駐**: 読取／書込速度・累計転送量のリアルタイム表示、「エクスプローラーで開く」、安全なワンクリック・アンマウント、システムトレイ常駐、および CLI コマンドプレビュー。
- **WinFsp 不要のオフライン機能**: WinFsp 未導入環境でも、ext2/3/4 イメージ内のフォルダ・ファイル閲覧（`ファイルブラウザ` タブ）や新規 `.img` 作成（`mkfs`）、WinFsp 導入ガイド表示を利用可能。
- **疎結合アーキテクチャ**: GPLv2 の `libext2fs` / `zeroext_core` を直接リンクせず、`zeroext.exe` をサブプロセス（JSON / Named Event IPC）として制御します。

## WinFsp のインストール（ドライブマウント実行時に必須）

`zeroext.exe mount` を使用して Windows ドライブレター（例: `M:`）へマウントするには、事前に **WinFsp** ランタイムをインストールする必要があります（オフラインコマンド `list` / `inspect` / `mkfs` / `ls` / `cat` / `write` 等は WinFsp なしでも動作します）。

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
zeroext.exe list
zeroext.exe inspect test.img

# クリーンな ext4 イメージ作成・操作
zeroext.exe mkfs test.img --size-mb 64 --label MYEXT4
zeroext.exe mkdir test.img /data
zeroext.exe write test.img /data/hello.txt "Hello from ZeroExt!"
zeroext.exe ls test.img /data
zeroext.exe cat test.img /data/hello.txt
zeroext.exe extents test.img /data/hello.txt

# WinFsp で Windows ドライブへマウント (WinFsp ランタイム要・既定は Read-Only + ダイレクトエクステント直読 ON)
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M:

# Direct I/O を明示的に無効化して標準 LRU キャッシュ経由でマウント
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --no-direct-io

# Read-Write マウント（明示的に --rw を指定・Direct I/O は自動無効）
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --rw

# ext4 は Read-Only + 直読 (既定 ON) に保ち、書き込みだけ Windows NTFS フォルダへ逃がす Overlay マウント
zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --overlay D:\ext4_upper
```

## ライセンス

本プロジェクトは `e2fsprogs` (`libext2fs`) に基づくため **GNU General Public License v2.0 (GPLv2)** の下で提供されます。詳細は [LICENSE](LICENSE) を参照してください。
