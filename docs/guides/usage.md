# 利用ガイド

本書は **ZeroExt4**（`ZeroExt4Gui.exe` デスクトップアプリおよび `zeroext4.exe` CLI／WinFsp マウントドライバ）を日常的に利用するための、WinFsp ランタイム導入、GUI 操作、および CLI コマンドリファレンスをまとめたガイドです。

- ソースコードからのビルド手順・`e2fsprogs` 更新・Perfetto トレース取得については [開発・ビルド・保守ガイド](development.md) を参照してください。
- 自動テスト体系・Sanitizer・静的解析仕様については [テスト・検証ガイド](../testing.md) を参照してください。

---

## 1. WinFsp ランタイムのインストール

> [!IMPORTANT]
> 実機のドライブレター（例: `L:`）や NTFS 空フォルダへマウントするには、事前に **WinFsp ランタイム** がインストールされている必要があります（`zeroext4.exe` が実行時に `winfsp-x64.dll` を自動検出・ロードします）。
> なお、`ZeroExt4Gui.exe` の「ファイルブラウザ」タブや CLI のオフラインコマンド（`list` / `inspect` / `doctor` / `mkfs` / `ls` / `cat` / `write` 等）は WinFsp なしでも動作します。

### 1.1 インストール方法

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

### 1.2 インストール確認

`winfsp-x64.dll` およびレジストリキーの存在を確認します：

```powershell
Test-Path "C:\Program Files (x86)\WinFsp\bin\winfsp-x64.dll"
Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\WinFsp" -Name InstallDir
```

---

## 2. GUI アプリケーション（`ZeroExt4Gui.exe`）の使い方

`.\build\gui\ZeroExt4Gui.exe` を起動すると、CLI コマンドを意識せずに ext2 / ext3 / ext4 ボリュームの検査・マウント・管理を行えます。

- **表示言語（英語 / 日本語）切替**:
  - 既定では OS ロケールに応じて自動選択され、ヘッダーの言語メニュー（`🌐 Auto (JA)` / `🌐 Auto (EN)` / `🌐 English` / `🌐 日本語`）から再起動なしで即時切り替えできます。
- **ソース追加・検査**:
  - 起動時に接続中の物理ディスクを自動スキャンします（非管理者権限の場合はツールバーの **「管理者権限で再起動」** から UAC 昇格できます）。
  - `.img` / `.raw` ファイルの追加（**「イメージを追加...」** ボタンまたはドラッグ＆ドロップ）と新規 ext4 イメージ作成（**「新規作成...」**）は左ペイン下部から実行できます（非管理者権限のまま利用可能）。
- **マウント設定タブ**:
  - モードカードから **Read-Only**（既定・Direct I/O ON）・**Overlay**（v2 `.wh.__ext4_lower_id` 事前検証・非空未識別フォルダ拒否ガード・**差分確認 / `.tar` エクスポート / 安全リセット** UI 付き）・**Read-Write**（Dirty／エラー／暗号化検出時は安全のため自動抑止し RO / Overlay を推奨）を選択し、空きドライブレター（既定は `L:` 優先）または空 NTFS フォルダへワンクリックでマウントします。
  - 詳細設定では Direct I/O 切替・大文字／小文字の区別（`--case-sensitive`）・LRU キャッシュブロック数・デバッグログ・Perfetto トレース出力・同等 CLI コマンドのコピーを行えます（「詳細 & ログ」タブの **「診断を実行 (`doctor`)」** ボタンから環境・ボリューム・Upperdir の一括診断も可能です）。
- **アクティブマウントタブ & システムトレイ**:
  - 稼働中のマウントごとの読取／書込スループット（MB/s）および累計転送量をリアルタイム表示し、「エクスプローラーで開く」や安全なアンマウントを実行できます。
  - マウント中にウィンドウを閉じた場合はシステムトレイへ最小化してマウントを維持できます（トレイメニューから再表示・開く・一括アンマウント終了が可能）。
  - 異常終了などで残留したドライブレターがある場合は **「残留ドライブ解除」** からクリーンアップできます（`QueryDosDeviceW` / `GetVolumeInformationW` により ZeroExt4/WinFsp 残留ドライブまたは `ext4` マウントのみを自動検出し、`C:` や通常の NTFS/exFAT ドライブ等は誤って解除されないよう保護されています）。
- **ファイルブラウザタブ（WinFsp 不要）**:
  - WinFsp が未導入の環境でも、`zeroext4 ls --json` を通じて ext2 / ext3 / ext4 内のディレクトリ・ファイル一覧を非破壊で閲覧できます。
- **ライト／ダークテーマ切替**:
  - 右上のテーマボタンから即座に切り替え可能です（設定およびボリュームごとのマウントプリセットは `%LOCALAPPDATA%\ZeroExt4\gui_settings.json` に保存されます）。

---

## 3. CLI リファレンス

### 3.1 オフライン検査・イメージ操作

```powershell
# 物理ドライブ一覧またはイメージファイルのパーティション解析・診断（管理者権限で物理ディスクアクセス可）
.\build\zeroext4.exe list
.\build\zeroext4.exe inspect <image_or_disk> [--partition <N>] [--json]
.\build\zeroext4.exe doctor [--source <image_or_disk>] [--partition <N>] [--overlay <ntfs_upper_dir>] [--json]

# Overlay Upperdir の差分確認・検証・.tar エクスポート・安全リセット
.\build\zeroext4.exe overlay status   --upper D:\ext4_upper [--lower <image_or_disk>] [--partition <N>] [--json]
.\build\zeroext4.exe overlay diff     --upper D:\ext4_upper [--lower <image_or_disk>] [--partition <N>] [--json]
.\build\zeroext4.exe overlay validate --upper D:\ext4_upper [--lower <image_or_disk>] [--partition <N>] [--json]
.\build\zeroext4.exe overlay export   --upper D:\ext4_upper --output overlay_diff.tar [--lower <image_or_disk>] [--json]
.\build\zeroext4.exe overlay reset    --upper D:\ext4_upper --yes [--json]

# ext4 イメージ新規作成（4KB ブロック・256B iノード・extents・64bit・metadata_csum 有効）
.\build\zeroext4.exe mkfs test.img --size-mb 64 --label MYEXT4

# ファイル操作（イメージまたは物理パーティションに対して直接実行）
.\build\zeroext4.exe mkdir test.img /data [--partition <N>]
.\build\zeroext4.exe write test.img /data/hello.txt "Hello ext4!" [--partition <N>]
.\build\zeroext4.exe write test.img /data/model.bin C:\path\to\local.bin --from-file [--partition <N>]
.\build\zeroext4.exe ls      test.img /data [--partition <N>]
.\build\zeroext4.exe stat    test.img /data/hello.txt [--partition <N>]
.\build\zeroext4.exe cat     test.img /data/hello.txt [--partition <N>]
.\build\zeroext4.exe extents test.img /data/model.bin [--partition <N>]
.\build\zeroext4.exe mv      test.img /data/hello.txt /data/renamed.txt [--partition <N>]
.\build\zeroext4.exe rm      test.img /data/renamed.txt [--partition <N>]
```

### 3.2 ドライブマウントとオプション一覧

> [!NOTE]
> 物理ディスク（`\\.\PhysicalDriveX`）を対象にする場合は、**管理者権限のターミナル** から実行してください。

> [!WARNING]
> **`--rw` モード利用時のデータ破壊リスク**: `--rw`（直接 Read-Write）は Linux カーネルのような JBD2 トランザクションジャーナリングを伴わず、`libext2fs` を通じて ext4 のメタデータおよびデータブロックを直接更新します。書込中の異常終了・強制終了・電源断や予期しないバグにより ext4 パーティション／イメージが破損するおそれがあります。重要なデータでは事前バックアップを取得するか、下位 ext4 を 1 バイトも書き換えない `--overlay` モードを使用してください（万一 `--rw` 書き込み中に異常終了した場合は、Linux 上で `e2fsck` による検査・修復を実施してください）。

```powershell
# Read-Only + Direct Extent I/O マウント（既定動作：フラグ不要・GGUF 等も高速かつ安全に読取可）
.\build\zeroext4.exe mount --source test.img --mountpoint L:

# Direct I/O を無効化して標準の 8MB ブロック LRU キャッシュ経路のみでマウント
.\build\zeroext4.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint L: --no-direct-io

# Read-Write マウント（既定は上流 libext2fs 準拠、Dirty Journal 検出時は自動拒否、Direct I/O は自動無効）
.\build\zeroext4.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint L: --rw

# Read-Write + バッチ連続エクステント高速 I/O（実験的オプトイン・独自経路のためテスト用途等向け）
.\build\zeroext4.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint L: --rw --rw-fast-io

# 非破壊 Overlay マウント（Lower の ext4 は常に RO＋Direct I/O 既定 ON、作成/変更は NTFS Upperdir へ保存）
# ※ 初回利用時に Upperdir に .wh.__ext4_lower_id を記録し、別の ext4 への誤再利用を自動ブロックします
.\build\zeroext4.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint L: --overlay D:\ext4_upper

# NTFS の空きディレクトリへ直接マウントすることも可能
# （親ディレクトリは通常の NTFS 書き込み領域、サブフォルダ配下だけを Overlay/ext4 として公開）
.\build\zeroext4.exe mount --source \\.\PhysicalDrive2 --partition 2 --mountpoint C:\models\linux_ext4
```

主なオプション一覧：
- `--rw`: ext4 への直接 Read-Write マウントを有効化（未指定時は Read-Only、`--overlay` とは併用不可、既定は上流 `e2fsprogs` 準拠）
- `--rw-fast-io`: `--rw` 併用時のみ有効な実験的オプション。`ext2fs_file_read`/`write` をバイパスし、独自のバッチ連続エクステント書込・`fs_lock` 外並列読込を有効化（想定外の不具合リスクを避けるため既定は無効）
- `--overlay <dir>`: NTFS Upperdir を指定して非破壊 Overlay RW マウント（Lower ext4 は厳密 RO、`--rw` とは併用不可、v2 `.wh.__ext4_lower_id` で UUID・GPT PartUUID・パーティション配置・`:cs` モードを照合し、マーカー無し非空フォルダは `-ENOTEMPTY` で拒否）
- `--case-sensitive`: ファイル名・ディレクトリ名の大文字・小文字を区別してマウント（`--overlay` 併用時は空の NTFS Upperdir に `FileCaseSensitiveInfo` 属性を設定し、`.wh.__ext4_lower_id` に `:cs` 識別子を記録）
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
- `--json`: `list` / `inspect` / `doctor` / `overlay` / `ls` サブコマンドで UTF-8 構造化 JSON 出力

### 3.3 シンボリックリンクと Windows 名前空間ガードの仕様

- **シンボリックリンク (WinFsp Reparse Point `SYMLINK`)**:
  - 通常の Read-Only / `--rw` マウントでは ext4 上のシンボリックリンクを Windows Reparse Point として公開します（ボリューム内の POSIX 絶対パスもボリューム相対パスへ自動変換）。ホスト側のパスへ越境する相対 `..` やドライブ文字・UNC を含むターゲットは安全のため解決を拒否します。
  - `--overlay` モードでは下位 ext4 の symlink の読取（`readlink`）および Whiteout による削除が可能ですが、symlink 自体の Copy-up 書き込み・リネーム・Upper への新規 symlink 作成は非対応（`-ENOTSUP`）となります。
  - また `--overlay` で下位 ext4 由来ディレクトリをリネームするとサブツリー全体が Upperdir へ Copy-up されるため、事前に空き容量検査・大容量警告・失敗時ロールバックが行われます。
- **Windows 非互換名と大小文字衝突**:
  - `CON`, `NUL`, `AUX`, `PRN`, `COM1..9`, `LPT1..9`（拡張子付き含む）や `< > : " | ? *` 等を含むパスは新規作成・リネーム・Upperdir Copy-up を安全に拒否します（Lower Read-Only 側での既存参照のみ維持）。
  - 既定の大文字小文字を区別しないマウント下で `Foo.txt` と `foo.txt` が共存する場合は、厳密一致を最優先しつつフォールバック時に辞書順最小名 → 最小 inode の決定論的順序で解決し警告ログを出力します（完全一致アクセスが必要な場合は `--case-sensitive` を指定してください）。

### 3.4 安全なアンマウントと管理者権限時のドライブ可視性

アンマウントは実行中のコンソールで `Ctrl+C` を押下します（`ext2fs_flush` と I/O キャッシュフラッシュを実行したうえで安全に停止・解除します）。

プロセスが強制終了や異常終了してドライブレターだけが残ってしまった場合は、マウント時と同じ権限（物理ディスクの場合は管理者権限）で以下を実行してドライブ定義を解除できます（安全のため `QueryDosDeviceW` と `GetVolumeInformationW` により対象ドライブを検査し、`C:` や通常の NTFS/exFAT ドライブ等、ZeroExt4/WinFsp 以外のボリュームは解除を拒否します）：

```powershell
.\build\zeroext4.exe unmount L:
```

> [!NOTE]
> **管理者権限で実行した場合のドライブ表示について**: 物理ディスクのマウントには管理者権限が必要です。昇格したプロセスが作るドライブレターは通常、昇格セッションからしか見えませんが、`zeroext4` は昇格を検知すると Mount Manager 経由のグローバルドライブ (`\\.\X:`) として登録し、失敗時はデスクトップユーザーのセッションにも定義します（[ADR-0009](../adr/0009-elevated-drive-letter-visibility.md)）。使用済みドライブレターの場合は空き文字を指定してください。
