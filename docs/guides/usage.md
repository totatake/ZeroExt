# 利用ガイド (`ZeroExt` / `zeroext`)

本書は **ZeroExt**（`zeroext.exe` CLI・WinFsp マウントドライバおよび `ZeroExtGui.exe` デスクトップアプリ）のビルド手順、GUI 操作、CLI リファレンス、WinFsp 導入・マウント／アンマウント運用、および `e2fsprogs` アップストリーム更新手順をまとめたガイドです。

- 自動テスト体系・Sanitizer・静的解析仕様については [テスト仕様・検証ガイド](../testing.md) を参照してください。
- 実測ベンチマーク・Perfetto 解析手順の詳細は [評価レポート一覧](../reports/README.md) を参照してください。

---

## 1. ビルド手順

### 必須要件（ビルド時）
- **Visual Studio 2022**（BuildTools / Community / Professional / Enterprise：`cl.exe`, `cmake.exe`, `ninja.exe`, `ctest.exe`）
- **Python 3.x**（`%LOCALAPPDATA%\Python\bin\python.exe` を優先検出し、`ext2_err.et.in` と `ext2_types.h.in` からヘッダを自動生成）
- **.NET 10 SDK**（任意：GUI アプリ `ZeroExtGui.exe` および `ZeroExt.Gui.Tests` をビルド・テストする場合）
- **WinFsp SDK はビルド時不要**（[winfsp.h](../../third_party/winfsp/include/winfsp/winfsp.h) に互換 ABI 定義と `winfsp-x64.dll` 動的ローダーを内蔵しているため）。実ドライブレターへマウントする実行時のみ、WinFsp ランタイムのインストールが必要です。

### ワンショットビルド＆テスト実行

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1 -Clean
```

生成物（`build/` 配下）：
- `build/ext2fs.lib`: e2fsprogs (`lib/et` + `lib/ext2fs`) 静的ライブラリ
- `build/zeroext_core.lib`: `win32_io` / `disk` / `vfs` / `extent` / `overlay` / `trace` / `winfsp_driver` コア静的ライブラリ
- `build/zeroext.exe`: CLI ＆ WinFsp マウント実行ファイル
- `build/zeroext_tests.exe`: 単体・結合テスト実行ファイル
- `build/zeroext_bench.exe`: Direct I/O 性能評価ベンチマーク実行ファイル（`tests/bench_direct_io.c`）
- `build/gui/ZeroExtGui.exe`: デスクトップ GUI マウントマネージャー（`.NET 10` / WPF、`scripts/build_gui.ps1` で単体ビルド可）

> `-SkipGui` スイッチを指定すると、C/CMake/CTest のみを実行します。

---

## 2. GUI アプリケーション (`ZeroExtGui.exe`) の使い方

`.\build\gui\ZeroExtGui.exe` を起動すると、CLI コマンドを意識せずに ext2 / ext3 / ext4 ボリュームの検査・マウント・管理を行えます。

- **表示言語（英語 / 日本語）切替**:
  - 既定では OS ロケールに応じて英語または日本語を自動選択し、画面上から再起動なしで即時切り替えできます（`v0.5.0` で追加）。
- **ソース追加・検査**:
  - 起動時に接続中の物理ディスクを自動スキャンします（非管理者権限の場合はツールバーの **「管理者権限で再起動」** から UAC 昇格できます）。
  - `.img` / `.raw` ファイルは **「イメージを追加...」** ボタンまたはウィンドウへのドラッグ＆ドロップで追加できます（非管理者権限のままマウント可能）。
  - **「新規作成...」** から新規 ext4 イメージを作成できます。
- **マウント設定タブ**:
  - モードカードから **Read-Only**（既定・Direct I/O ON）・**Overlay**（NTFS Upperdir の `.wh.__ext4_lower_id` 事前チェック機能付き）・**Read-Write**（Dirty 時の自動 RO フォールバック選択可）を選択し、空きドライブレターまたは空 NTFS フォルダへワンクリックでマウントします。
  - 詳細設定では Direct I/O 切替・LRU キャッシュブロック数・デバッグログ・Perfetto トレース出力・同等 CLI コマンドのコピーを行えます。
- **アクティブマウントタブ & システムトレイ**:
  - 稼働中のマウントごとの読取／書込スループット（MB/s）および累計転送量をリアルタイム表示し、「エクスプローラーで開く」や安全なアンマウントを実行できます。
  - マウント中にウィンドウを閉じた場合はシステムトレイへ最小化してマウントを維持できます（トレイメニューから再表示・開く・一括アンマウント終了が可能）。
  - 異常終了などで残留したドライブレターがある場合は **「残留ドライブ解除」** からクリーンアップできます（`QueryDosDeviceW` / `GetVolumeInformationW` により ZeroExt/WinFsp 残留ドライブまたは `ext4` マウントのみを自動検出し、`C:` や通常の NTFS/exFAT ドライブ等は誤って解除されないよう保護されています）。
- **ファイルブラウザタブ（WinFsp 不要）**:
  - WinFsp が未導入の環境でも、`zeroext ls --json` を通じて ext2 / ext3 / ext4 内のディレクトリ・ファイル一覧を非破壊で閲覧できます。
- **ライト／ダークテーマ切替**:
  - 右上のテーマボタンから即座に切り替え可能です（設定およびボリュームごとのマウントプリセットは `%LOCALAPPDATA%\ZeroExt\gui_settings.json` に保存されます）。

---

## 3. CLI リファレンス & WinFsp マウント手順

### 3.1 オフライン検査・イメージ操作

```powershell
# 物理ドライブ一覧またはイメージファイルのパーティション解析（管理者権限で物理ディスクアクセス可）
.\build\zeroext.exe list
.\build\zeroext.exe inspect <image_or_disk> [--partition <N>]

# ext4 イメージ新規作成（4KB ブロック・256B iノード・extents・64bit・metadata_csum 有効）
.\build\zeroext.exe mkfs test.img --size-mb 64 --label MYEXT4

# ファイル操作（イメージまたは物理パーティションに対して直接実行）
.\build\zeroext.exe mkdir test.img /data [--partition <N>]
.\build\zeroext.exe write test.img /data/hello.txt "Hello ext4!" [--partition <N>]
.\build\zeroext.exe write test.img /data/model.bin C:\path\to\local.bin --from-file [--partition <N>]
.\build\zeroext.exe ls      test.img /data [--partition <N>]
.\build\zeroext.exe stat    test.img /data/hello.txt [--partition <N>]
.\build\zeroext.exe cat     test.img /data/hello.txt [--partition <N>]
.\build\zeroext.exe extents test.img /data/model.bin [--partition <N>]
.\build\zeroext.exe mv      test.img /data/hello.txt /data/renamed.txt [--partition <N>]
.\build\zeroext.exe rm      test.img /data/renamed.txt [--partition <N>]
```

### 3.2 WinFsp のインストールと Windows ドライブレターへのマウント

> [!IMPORTANT]
> 実機のドライブレター（例: `M:`）としてマウントするには、事前に **WinFsp ランタイム** がインストールされている必要があります（`zeroext.exe` が実行時に `winfsp-x64.dll` を自動検出・ロードします）。
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
# Read-Only + Direct Extent I/O マウント（既定動作：フラグ不要・GGUF 等も高速かつ安全に読取可）
.\build\zeroext.exe mount --source test.img --mountpoint M:

# Direct I/O を無効化して標準の 8MB ブロック LRU キャッシュ経路のみでマウント
.\build\zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --no-direct-io

# Read-Write マウント（明示的に --rw を指定、Dirty Journal 検出時は自動拒否、Direct I/O は自動無効）
.\build\zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --rw

# 非破壊 Overlay マウント（Lower の ext4 は常に RO＋Direct I/O 既定 ON、作成/変更は NTFS Upperdir へ保存）
# ※ 初回利用時に Upperdir に .wh.__ext4_lower_id を記録し、別の ext4 への誤再利用を自動ブロックします
.\build\zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint M: --overlay D:\ext4_upper

# NTFS の空きディレクトリへ直接マウントすることも可能
# （親ディレクトリは通常の NTFS 書き込み領域、サブフォルダ配下だけを Overlay/ext4 として公開）
.\build\zeroext.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint C:\models\linux_ext4
```

主なオプション一覧：
- `--rw`: ext4 への直接 Read-Write マウントを有効化（未指定時は Read-Only、`--overlay` とは併用不可）
- `--overlay <dir>`: NTFS Upperdir を指定して非破壊 Overlay RW マウント（Lower ext4 は厳密 RO、`--rw` とは併用不可）
- `--no-direct-io`: RO / `--overlay` 時の既定 Direct Extent I/O を無効化し、標準ブロックキャッシュ経路を使用
- `--direct-io`: Direct Extent I/O を明示有効化（RO / `--overlay` では既定 ON のため通常不要、`--rw` とは併用不可）
- `--cache-blocks <N>`: LRU ブロックキャッシュ数（既定 2048 ブロック = 8MB @ 4KB）
- `--file-info-timeout <ms|infinite>`: WinFsp カーネルメタデータ／キャッシュ有効時間（RO 時は既定 `infinite`）
- `--ro-fallback`: `--rw` 指定時に Dirty Journal 検出時へ RO 自動フォールバック
- `--force-dirty`: Dirty Journal 検出時でも強制的に RW オープン（非推奨・上級者向け）
- `--debug`: WinFsp デバッグログ有効化
- `--stop-event <name>`: 指定 Win32 Event シグナル受信時に安全アンマウント
- `--json-events`: マウント状態・統計・エラーを 1 行 JSON イベントで標準出力へ通知
- `--stats-interval <ms>`: `--json-events` 併用時の I/O 統計イベント出力間隔 (ms)
- `--trace <path.json>`: [Perfetto UI](https://ui.perfetto.dev/) 互換 Chrome Trace Event JSON を出力
- `--trace-events <N>`: スレッドごとのトレースリングバッファ容量（既定 65536、2 のべき乗で `1024..1048576`）
- `--json`: `list` / `inspect` / `ls` サブコマンドで UTF-8 構造化 JSON 出力

#### 安全なアンマウント・ドライブ可視性

アンマウントは実行中のコンソールで `Ctrl+C` を押下します（`ext2fs_flush` と I/O キャッシュフラッシュを実行したうえで安全に停止・解除します）。

プロセスが強制終了や異常終了してドライブレターだけが残ってしまった場合は、マウント時と同じ権限（物理ディスクの場合は管理者権限）で以下を実行してドライブ定義を解除できます（安全のため `QueryDosDeviceW` と `GetVolumeInformationW` により対象ドライブを検査し、`C:` や通常の NTFS/exFAT ドライブ等、ZeroExt/WinFsp 以外のボリュームは解除を拒否します）：

```powershell
.\build\zeroext.exe unmount M:
```

> [!NOTE]
> **管理者権限で実行した場合のドライブ表示について**: 物理ディスクのマウントには管理者権限が必要です。昇格したプロセスが作るドライブレターは通常、昇格セッションからしか見えませんが、`zeroext` は昇格を検知すると Mount Manager 経由のグローバルドライブ (`\\.\X:`) として登録し、失敗時はデスクトップユーザーのセッションにも定義します（[ADR-0009](../adr/0009-elevated-drive-letter-visibility.md)）。使用済みドライブレターの場合は空き文字を指定してください。

---

## 4. 性能ベンチマーク (`zeroext_bench.exe`) と Perfetto 性能トレース

Direct I/O の有無による VFS 単体層・WinFsp 実マウント経由のスループット比較には `.\build\zeroext_bench.exe` を、同一 NVMe SSD 上での Windows Native (NTFS) / `ZeroExt` / `wsl --mount --vhd` 横並び比較には管理者権限 PowerShell 用の `scripts/bench_compare_ssd.ps1` を使用します。詳細な測定結果・考察・再現手順は [性能評価報告書](../reports/performance-evaluation.md) および [Perfetto トレース定量解析・性能改善計画](../reports/perfetto-trace-analysis.md) を参照してください。

`zeroext.exe mount` または `zeroext_bench.exe` に `--trace <output.json>` を指定すると、スレッド別のコールバック・ロック競合・I/O スパンおよびキャッシュカウンタを Chrome Trace Event JSON 形式で記録します（未指定時は単一フラグ分岐のみでオーバーヘッドゼロ）。出力された `.json` ファイルはブラウザで [Perfetto UI](https://ui.perfetto.dev/) にドラッグ＆ドロップしてタイムライン解析できます。

```powershell
# マウント中の動作をトレース（Ctrl+C またはアンマウント時に書き出し）
.\build\zeroext.exe mount --source test.img --mountpoint M: --trace .\zeroext_mount_trace.json

# ベンチマーク実行中のロック競合・Direct I/O・LRU キャッシュ挙動をトレース
.\build\zeroext_bench.exe --trace .\zeroext_bench_trace.json
```

---

## 5. `e2fsprogs` アップストリーム更新手順

本プロジェクトでは `compat/` および `scripts/generate_e2fsprogs_headers.py` へ互換レイヤーを分離した上で（[ADR-0005](../adr/0005-zero-patch-e2fsprogs-compat-layer.md)）、実上流の `e2fsprogs`（現在同梱版: `1.47.4`）を組み込んでいます（MSVC C11 コンパイル互換のための最小限調整のみ [scripts/update_e2fsprogs.ps1](../../scripts/update_e2fsprogs.ps1) で自動適用）。

新しい `e2fsprogs` バージョンを展開したディレクトリ `<path>` を指定して以下を実行すると、ソース同期・ヘッダ自動生成・ビルド・`ctest` 検証までを一括実行できます：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\update_e2fsprogs.ps1 -UpstreamDir <path\to\e2fsprogs-x.y.z>
```
