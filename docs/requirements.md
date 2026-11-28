# 要件定義書

## 1. 背景と目的

Windows 11 / 10 環境において、別ドライブや仮想イメージファイル（`.img`）に格納された Linux ファイルシステム（`ext2` / `ext3` / `ext4`）を、通常の Windows ドライブレターとして高速かつ安全にマウント・読み書き（Read-Only / Overlay / Read-Write およびメモリマップドファイル `mmap` 対応）できるユーザー空間ファイルシステムドライバ **`ZeroExt4`（CLI 実行ファイル: `zeroext4.exe`、GUI: `ZeroExt4Gui.exe`）** を構築する。

従来の純粋な Windows カーネルドライバ（`.sys`）方式は、カーネル空間での C ランタイム非互換・Cache Manager (`Cc`) / Memory Manager (`Mm`) 連携の実装コスト・EV コード署名および Microsoft Attestation Signing 要件・バグ発生時の BSOD リスクが大きい。一方、軽量組み込み向けライブラリ（`lwext4` 等）は省メモリ優先であり、最新 ext4 機能（`64bit`・`metadata_csum`・完全な JBD2 状態判定・大容量転送時のキャッシュ/並列性能）に制約がある。

そのため本プロジェクトでは、**WinFsp（Windows File System Proxy）** をカーネル側の土台に、本家 `e2fsprogs` の **`libext2fs`** をユーザー空間エンジンとして組み合わせることで、**ドライバ署名不要・BSOD リスクゼロ・高い ext4 互換性と転送性能** の両立を目的とする。

---

## 2. スコープと前提

### 2.1 対応スコープ
- **対象環境**: Windows 10 / 11 x64
- **対象ファイルシステム**: Linux `ext2` / `ext3` / `ext4`
- **対象メディア**: Windows 物理ディスク（`\\.\PhysicalDrive0..15`）および生ディスク/パーティションイメージファイル（`.img` / `.raw`）
- **対象パーティション方式**: GPT（512B / 4096B セクタ）、MBR（基本パーティション 1〜4）、EBR（論理パーティション 5 以降）、Raw 未パーティションイメージ
- **提供形態**: Windows ユーザー空間 CLI・WinFsp マウントサービス（`zeroext4.exe`）、デスクトップ GUI マウントマネージャー（`ZeroExt4Gui.exe`）、および再利用可能な静的ライブラリ（`ext2fs.lib`, `zeroext4_core.lib`）

### 2.2 プロジェクト境界とライセンス前提
- Read-Only 専用・外部ドライバ不要を旨とする他プロジェクト（`LinuxRoDrive` 等）とはコードベース・安全性モデル・ビルド環境・ライセンスを分離し、独立プロジェクトとして維持する（連携する場合も CLI サブプロセス呼び出し等の疎結合で扱う）。
- `e2fsprogs` (`libext2fs`) に基づくため、ネイティブコアおよびバイナリは **GNU General Public License v2.0 (GPLv2)** に準拠する（GUI は `zeroext4.exe` をサブプロセスとして制御し、ネイティブリンクを行わない）。

---

## 3. 機能要件

| ID | 要件名 | 詳細仕様 |
| :--- | :--- | :--- |
| **REQ-F-001** | **ディスク・パーティション認識** | 物理ディスク (`\\.\PhysicalDrive0..15`) およびイメージファイル (`.img`) に対し、GPT（保護 MBR 認識および 512B/4096B セクタ自動判定）、MBR、EBR 論理パーティション（循環参照検出付き）、および Raw 未パーティションイメージを解析できること。 |
| **REQ-F-002** | **スーパーブロック＆ジャーナル状態検査** | ext2 / ext3 / ext4 スーパーブロック（`+1024B`, Magic `0xEF53`）を解析し、FS 種別・ブロックサイズ・iノードサイズ・総容量/空き容量・ラベル・UUID・Feature フラグおよび **JBD2 ジャーナル状態（`NONE` / `CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`）** をマウント前に判定できること。 |
| **REQ-F-003** | **セクタ境界・パーティション境界保護 I/O** | `CreateFileW` + `FILE_FLAG_OVERLAPPED` によりオフセット指定並列 Read/Write を行い、非セクタ境界 I/O に対する自動 Read-Modify-Write (RMW)、パーティション範囲 `[offset, offset+size)` 逸脱の完全ブロック、およびスレッドセーフ・シャード化ブロックキャッシュを提供すること。 |
| **REQ-F-004** | **Read-Only / Read-Write 制御と Dirty / 未対応機能保護** | RO マウント時は `GENERIC_READ` のみでオープンし、I/O 層で書き込みを物理遮断する。RW マウント要求時に Dirty Journal (`EXT3_FEATURE_INCOMPAT_RECOVER` / JBD2 `s_start != 0` / `EXT2_ERROR_FS`)、または外部ジャーナル (`s_journal_dev`) / `encrypt` (`fscrypt`) / `verity` / `shared_blocks` / 未知 `ro_compat` を検出した場合は RW マウントを拒否（または Dirty 時の `--ro-fallback` により RO 自動フォールバック）すること。RO / Overlay 時は `--ignore-csum-errors` による救出マウントを許可し、`--rw` との併用は拒否すること。 |
| **REQ-F-005** | **最新 ext4 ファイルシステム操作 (VFS)** | 64bit ブロック番号（`blk64_t`）、`EXT4_EXTENTS_FL`（インラインおよびマルチレベルエクステント）、従来型間接ブロック、256B+ iノード（ナノ秒精度タイムスタンプ・`crtime`）、`metadata_csum`（CRC32C）検証・更新、複数ブロックディレクトリ拡張・HTree 互換ディレクトリ操作、シンボリックリンク（Fast / Block および WinFsp Reparse Point 透過連携）、リネーム、トランケート、ボリュームラベル変更、Windows 予約名／禁止文字ガード、および `--case-sensitive` による大小文字区別切替（既定 CI 時の決定論的衝突解決含む）に対応すること。 |
| **REQ-F-006** | **WinFsp ユーザー空間ドライバ統合** | `FSP_FILE_SYSTEM_INTERFACE` の全コールバック（`ReparsePoints` を含む）を実装し、エクスプローラー・Win32 アプリ・メモリマップドファイル (`mmap`) からドライブレター（既定 `L:` 優先）または空 NTFS フォルダとして透過アクセス可能にすること。`winfsp-x64.dll` は実行時に動的ロードし、SDK ヘッダ非導入環境でもビルド・インプロセス検証できること。 |
| **REQ-F-007** | **CLI 管理・オフライン操作・診断ツール (`zeroext4.exe`)** | `list`, `inspect`（`mount_advice`・拡張機能名含む）, `fsck-lite`（4 パス非破壊整合性診断）, `doctor`（環境・ボリューム・Upperdir 統合診断）, `overlay`（`status` / `diff` / `validate` / `export` / `reset`）, `mkfs`, `ls`, `stat`, `cat`, `write`, `mkdir`, `rm`, `mv`, `extents`, `mount`（`--rw-fast-io` オプトイン含む）, `unmount` サブコマンドおよび `--json` / `--json-events` を備え、イメージ生成・オフラインファイル操作・非破壊診断・安全検証付き `unmount`（WinFsp/ext4 ターゲット判定）を実行できること。 |
| **REQ-F-008** | **ダイレクト・エクステント I/O エンジン (既定有効 / `--no-direct-io` で無効化)** | Read-Only マウント（既定の RO 動作）および `--overlay`（Lower が RO の場合）において既定でダイレクト・エクステント I/O を有効化し、`--no-direct-io` 指定時のみ標準ブロックキャッシュ経路へ切り替えられること（`--rw` マウント時は自動無効化、かつ `--direct-io` 明示併用は `-EINVAL` で拒否。`--rw` 向けの高速バッチ経路は明示オプトイン `--rw --rw-fast-io` に分離）。初回読み取り時にエクステントツリー（多段ツリー含む）・間接ブロック・インラインデータを抽出してキャッシュし、以降の `Read` 要求を `libext2fs` およびメタデータロックをバイパスして二分探索 `O(log N)` で物理セクタへ直読すること（未初期化エクステント・スパースホールのゼロ埋めを含む）。 |
| **REQ-F-009** | **NTFS Upperdir オーバーレイマウント (`--overlay`)** | Lower 側の ext4 ボリュームを厳密に Read-Only で開いたまま（Dirty Journal ボリュームも安全に参照可）、Windows NTFS 上の指定ディレクトリを Upper 層として重ね合わせ、新規作成・変更（Copy-up）・削除（`.wh.<name>` ホワイトアウト / `.wh.__dir_opaque` 不透過ディレクトリ）・リネーム（事前ツリー計測・空き容量検査・失敗時ロールバック付き）を O(1) ハッシュマップと決定論的合成 iノードにより非破壊の Read-Write ボリュームとして提供すること。v2 `.wh.__ext4_lower_id`（UUID・レイアウト・`--case-sensitive` `:cs` 識別子）照合および非空未識別フォルダ拒否により、異なる Lower・異なる大小文字モード間での Upperdir 誤流用を防止すること。 |
| **REQ-F-010** | **デスクトップ GUI マウントマネージャー (`ZeroExt4Gui.exe`)** | `.NET 10` WPF によるディスク／`.img` 一覧・JBD2 状態・`mount_advice` 可視化（Dirty/エラー時の RW 自動抑止）、RO / Overlay / RW マウント切替（`--case-sensitive`・`--rw-fast-io` 対応）、`doctor` / `fsck-lite` 診断実行、Overlay 差分プレビュー／`.tar` エクスポート／安全リセット、システムトレイ常駐、安全検証付き残留ドライブ解除、オフラインファイルブラウザ、ライト／ダークテーマ切替、および英語／日本語 i18n 即時切替を提供すること。GPLv2 の `libext2fs` を直接リンクせず `zeroext4.exe` を JSON / Named Event IPC 経由のサブプロセスとして制御すること。 |
| **REQ-F-011** | **Perfetto 性能トレース出力 (`--trace`)** | 未指定時は単一分岐でゼロオーバーヘッドを維持し、`--trace <path.json>` 指定時に WinFsp コールバック、`SRWLOCK` 待ち／保持、キャッシュヒット、I/O スパンをスレッドローカルリングバッファへ記録して Chrome Trace Event JSON（[Perfetto UI](https://ui.perfetto.dev/) 対応）として出力できること。 |

---

## 4. 非機能要件

| ID | カテゴリ | 要件内容 |
| :--- | :--- | :--- |
| **REQ-NF-001** | **性能（並列 Read・ダイレクト I/O・キャッシュ）** | エクステント直結エンジンと `win32_io_pread_direct` により、抽出済みの通常ファイル `Read` は `ctx->rwlock` や I/O ブロックキャッシュロックを取らず並列直読でき、GGUF 等の大容量 `mmap` ロードにおいてネイティブディスクに近いスループットを実現する。メタデータ層は 16 シャード正/負 Dentry キャッシュ、2 階層 `SRWLOCK`、および WinFsp `FileInfoTimeout = INFINITE` カーネルキャッシュで高速化する。 |
| **REQ-NF-002** | **安全性・耐障害性** | ユーザー空間プロセスとして動作し、Windows 11 のドライバ署名（Attestation Signing）不要で動作し、異常時も OS クラッシュ（BSOD）を起こさない。 |
| **REQ-NF-003** | **アップストリーム追従性** | `third_party/e2fsprogs` へ Windows 固有パッチ（`#ifdef _MSC_VER` 等）を混入させず、`compat/` と自動ヘッダ生成スクリプトへ完全分離することで、将来の `e2fsprogs` 新バージョンへの追従・ビルド検証を容易にする。 |
| **REQ-NF-004** | **ビルド可搬性・自己完結性** | Cygwin / MSYS2 ランタイム DLL（`cygwin1.dll` 等）に依存せず、Visual Studio 2022 (`cl.exe` / CMake / Ninja) のみで純粋なネイティブ Windows バイナリとしてビルドできること。 |

---

## 5. 今後の拡張構想

リリース済み機能の詳細は [CHANGELOG.md](../CHANGELOG.md)、今後の開発計画および未実装の機能拡張（Phase 1〜4）は [ロードマップ](roadmap.md) を参照してください。
