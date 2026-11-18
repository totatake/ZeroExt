# 要件定義書 (`libext2fs-win` / `ext4fsp`)

## 1. 背景と目的

Windows 11 環境において、別ドライブや仮想イメージファイル（`.img`）に格納された Linux ファイルシステム（`ext2` / `ext3` / `ext4`）を、通常の Windows ドライブレターとして高速かつ安全にマウント・読み書き（Read-Only / Read-Write およびメモリマップドファイル `mmap` 対応）できるユーザー空間ファイルシステムドライバ **`libext2fs-win`（実行ファイル名: `ext4fsp.exe`）** を構築する。

従来の純粋な Windows カーネルドライバ（`.sys`）方式は、カーネル空間での C ランタイム非互換・Cache Manager (`Cc`) / Memory Manager (`Mm`) 連携の実装コスト・EV コード署名および Microsoft Attestation Signing 要件・バグ発生時の BSOD リスクが大きい。一方、軽量組み込み向けライブラリ（`lwext4` 等）は省メモリ優先であり、最新 ext4 機能（`64bit`・`metadata_csum`・完全な JBD2 状態判定・大容量転送時のキャッシュ/並列性能）に制約がある。

そのため本プロジェクトでは、**WinFsp（Windows File System Proxy）** をカーネル側の土台に、本家 `e2fsprogs` の **`libext2fs`** をユーザー空間エンジンとして組み合わせることで、**ドライバ署名不要・BSOD リスクゼロ・高い ext4 互換性と転送性能** の両立を目的とする。

---

## 2. スコープと前提

### 2.1 対応スコープ
- **対象ファイルシステム**: Linux `ext2` / `ext3` / `ext4`
- **対象メディア**: Windows 物理ディスク（`\\.\PhysicalDrive0..15`）および生ディスク/パーティションイメージファイル（`.img`）
- **対象パーティション方式**: GPT（512B / 4096B セクタ）、MBR（基本パーティション 1〜4）、EBR（論理パーティション 5 以降）、Raw 未パーティションイメージ
- **実行形態**: Windows ユーザー空間 CLI・マウントサービス（`ext4fsp.exe`）および再利用可能な静的ライブラリ（`ext2fs.lib`, `ext4fsp_core.lib`）

### 2.2 プロジェクト境界とライセンス前提
- Read-Only 専用・外部ドライバ不要を旨とする他プロジェクト（`LinuxRoDrive` 等）とはコードベース・安全性モデル・ビルド環境・ライセンスを分離し、独立プロジェクトとして維持する（将来連携する場合も CLI サブプロセス呼び出し等の疎結合で扱う）。
- `e2fsprogs` (`libext2fs`) に基づくため、本プロジェクトは **GNU General Public License v2.0 (GPLv2)** に準拠する。

---

## 3. 機能要件

| ID | 要件名 | 詳細仕様 |
| :--- | :--- | :--- |
| **FR-01** | **ディスク・パーティション認識** | 物理ディスク (`\\.\PhysicalDrive0..15`) およびイメージファイル (`.img`) に対し、GPT（保護 MBR 認識および 512B/4096B セクタ自動判定）、MBR、EBR 論理パーティション（循環参照検出付き）、および Raw 未パーティションイメージを解析できること。 |
| **FR-02** | **スーパーブロック＆ジャーナル状態検査** | ext2 / ext3 / ext4 スーパーブロック（`+1024B`, Magic `0xEF53`）を解析し、FS 種別・ブロックサイズ・iノードサイズ・総容量/空き容量・ラベル・UUID・Feature フラグおよび **JBD2 ジャーナル状態（`NONE` / `CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`）** をマウント前に判定できること。 |
| **FR-03** | **セクタ境界・パーティション境界保護 I/O** | `CreateFileW` + `OVERLAPPED` によりオフセット指定 Read/Write を行い、非セクタ境界 I/O に対する自動 Read-Modify-Write (RMW)、パーティション範囲 `[offset, offset+size)` 逸脱の完全ブロック、およびスレッドセーフ LRU ブロックキャッシュを提供すること。 |
| **FR-04** | **Read-Only / Read-Write 制御と Dirty 保護** | RO マウント時は `GENERIC_READ` のみでオープンし、I/O 層で書き込みを物理遮断する。RW マウント要求時に Dirty Journal (`EXT3_FEATURE_INCOMPAT_RECOVER` / JBD2 `s_start != 0` / `EXT2_ERROR_FS`) を検出した場合は RW マウントを拒否（または設定により RO 自動フォールバック）すること。 |
| **FR-05** | **最新 ext4 ファイルシステム操作 (VFS)** | 64bit ブロック番号（`blk64_t`）、`EXT4_EXTENTS_FL`（インラインおよびマルチレベルエクステント）、従来型間接ブロック、256B+ iノード（ナノ秒精度タイムスタンプ・`crtime`）、`metadata_csum`（CRC32C）検証・更新、複数ブロックディレクトリ拡張・HTree 互換ディレクトリ操作、シンボリックリンク（Fast / Block）、リネーム、トランケート、ボリュームラベル変更に対応すること。 |
| **FR-06** | **WinFsp ユーザー空間ドライバ統合** | `FSP_FILE_SYSTEM_INTERFACE` の全コールバックを実装し、エクスプローラー・Win32 アプリ・メモリマップドファイル (`mmap`) からドライブレターとして透過アクセス可能にすること。`winfsp-x64.dll` は実行時に動的ロードし、SDK ヘッダ非導入環境でもビルド・インプロセス検証できること。 |
| **FR-07** | **CLI 管理・オフライン操作ツール (`ext4fsp.exe`)** | `list`, `inspect`, `mkfs`, `ls`, `stat`, `cat`, `write`, `mkdir`, `rm`, `mv`, `extents`, `mount`, `unmount` サブコマンドを備え、WinFsp マウントだけでなくイメージ生成・オフラインファイル操作・エクステントマッピング検査・診断を単体で実行できること。 |
| **FR-08** | **ダイレクト・エクステント I/O パススルー (`--direct-io`)** | Read-Only マウント（既定の RO 動作、または `--overlay` で下位が RO の場合）において `--direct-io` が明示指定された場合のみ有効化され（`--rw` マウントとの併用は拒否）、ファイルの初回読み取り時にエクステントツリー（多段ツリー含む）・間接ブロック・インラインデータを抽出してキャッシュし、以降の `Read` 要求を `libext2fs` およびメタデータロックをバイパスして二分探索 `O(log N)` で物理セクタへ直読すること（未初期化エクステント・スパースホールのゼロ埋めを含む）。 |
| **FR-09** | **NTFS Upperdir オーバーレイマウント (`--overlay`)** | Lower 側の ext4 ボリュームを厳密に Read-Only で開いたまま（Dirty Journal ボリュームも安全に参照可）、Windows NTFS 上の指定ディレクトリを Upper 層として重ね合わせ、新規作成・変更（Copy-up）・削除（`.wh.<name>` ホワイトアウト / `.wh.__dir_opaque` 不透過ディレクトリ）・リネームを非破壊の Read-Write ボリューム（ドライブレターまたは NTFS 空フォルダマウントポイント）として提供すること。Lower 識別ファイル (`.wh.__ext4_lower_id`) により異なる Lower への既存 Upperdir の誤流用を防止すること。 |

---

## 4. 非機能要件

| ID | カテゴリ | 要件内容 |
| :--- | :--- | :--- |
| **NFR-01** | **性能（並列 Read・ダイレクト I/O・キャッシュ）** | エクステント直結エンジンと `win32_io_pread_direct` により、抽出済みの通常ファイル `Read` は `ctx->rwlock` や I/O ブロックキャッシュロックを取らず並列直読でき、GGUF 等の大容量 `mmap` ロードにおいてネイティブディスクに近いスループットを実現する。メタデータ層は 2 階層 `SRWLOCK`＋LRU キャッシュおよび WinFsp `FileInfoTimeout` カーネルキャッシュで高速化する。 |
| **NFR-02** | **安全性・耐障害性** | ユーザー空間プロセスとして動作し、Windows 11 のドライバ署名（Attestation Signing）不要で動作し、異常時も OS クラッシュ（BSOD）を起こさない。 |
| **NFR-03** | **アップストリーム追従性** | `third_party/e2fsprogs` へ Windows 固有パッチ（`#ifdef _MSC_VER` 等）を混入させず、`compat/` と自動ヘッダ生成スクリプトへ完全分離することで、将来の `e2fsprogs` 新バージョンへの追従・ビルド検証を容易にする。 |
| **NFR-04** | **ビルド可搬性・自己完結性** | Cygwin / MSYS2 ランタイム DLL（`cygwin1.dll` 等）に依存せず、Visual Studio 2022 (`cl.exe` / CMake / Ninja) のみで純粋なネイティブ Windows バイナリとしてビルドできること。 |
