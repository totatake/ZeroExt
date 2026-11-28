# ロードマップ

本書は **ZeroExt4**（`v0.11.1` 時点：`zeroext4.exe` ユーザー空間ドライバ／CLI ＆ `ZeroExt4Gui.exe` デスクトップアプリ）の完了済みマイルストーンと、今後の安全強化・運用性向上・ext4 互換性拡充のロードマップを管理するものである。詳細な変更履歴は [CHANGELOG.md](../CHANGELOG.md) を参照されたい。

---

## 1. 完了済みマイルストーン

| バージョン | 完了日 | 主な達成事項 |
| :--- | :--- | :--- |
| `v0.1.0` | 2026-11-19 | `ZeroExt4`（`zeroext4.exe`）初回リリース。`libext2fs` + WinFsp 基盤、RO / Overlay 既定 Direct Extent I/O、NTFS `--overlay`（`.wh.__ext4_lower_id`）、Dirty Journal 保護、`UmFileContextIsUserContext2 = 1`、同一 SSD ベンチマーク |
| `v0.2.0` | 2026-11-19 | デスクトップ GUI マウントマネージャー `ZeroExt4Gui.exe`（.NET 10 WPF・疎結合 JSON / Named Event IPC）および `ZeroExt4.Gui.Tests` |
| `v0.3.0` | 2026-11-21 | `--trace` Perfetto 出力、Step 1〜4 性能最適化（`OVERLAPPED` + `INFINITE` Timeout + シャード化キャッシュ + Dentry キャッシュ + `FspGetDirInfoByName`）、ASan / Debug CRT リーク / `/analyze` 検証基盤 |
| `v0.4.0` | 2026-11-21 | GitHub Actions CI & タグ自動リリース、VS ツールチェーン自動検出、`LICENSE` (GPLv2) / `THIRD_PARTY_NOTICES.md`、日英バイリンガル `README` |
| `v0.5.0` | 2026-11-21 | 同梱 `e2fsprogs` `1.47.4` 追従（`fs_lock` 導入）・ベンチマーク再評価、`ZeroExt4Gui.exe` 英語／日本語ライブ切替 (i18n)、Phase 1〜4 ロードマップ拡充 |
| `v0.6.0` | 2026-11-21 | CLI `unmount` および GUI 残留ドライブ解除における `QueryDosDeviceW` / `GetVolumeInformationW` ターゲット安全性検証と単体／GUI テスト追加 |
| `v0.7.0` | 2026-11-22 | OSS リポジトリ構造標準化（`CHANGELOG.md` / `CONTRIBUTING.md` / `SECURITY.md` / `AGENTS.md`）、`docs/` の kebab-case 再編、`ADR-0001`〜`ADR-0013` 抽出、CI / Release ワークフロー分離 |
| `v0.8.0` | 2026-11-23 | `--case-sensitive` モード実装（NTFS Upperdir `FileCaseSensitiveInfo`・`:cs` マーカーガード・事前検証／エラー翻訳・`_cs` 推奨パス）、`L:` 優先統一、Dark モード `ComboBox` 修正・GUI ヘッダー見切れ解消 |
| `v0.8.1` | 2026-11-25 | ドキュメント体系整理（H1 タイトル・章番号・リンク名統一、`docs/guides/development.md`・`guides/README.md` 分離、要件 ID の `REQ-F-*`/`REQ-NF-*` 移行、`README`/`overview.md` 実測値同期・重複整理） |
| `v0.9.0` | 2026-11-26 | プロジェクト名・バイナリ・CMake ターゲット／オプション・WPF GUI・設定パスを `ZeroExt4` へ全面リネーム（破壊的変更）、[Ext4Windows との技術比較分析](reports/ext4windows-comparison.md) 追加 |
| `v0.10.0` | 2026-11-27 | `--rw` / 標準経路のバッチ連続エクステント書込・`fs_lock` 外並列読込・整列ゼロコピー書込最適化、`--rw-bench` ＆一貫性テスト追加、同一 SSD / WinFsp 実マウント / VFS ベンチマーク再評価 |
| `v0.10.1` | 2026-11-27 | 独自バッチ連続エクステント読込／書込を新オプトイン **`--rw-fast-io`** へ分離し、既定 `--rw` / `--no-direct-io` を `e2fsprogs` 準拠へ復元（GUI / `--rw-bench` 比較含む）、多段エクステント境界ホール修正、`FspRename` 既存ディレクトリ上書拒否修正、`fsx` 型ファザー・10 スレッド並列 RW ストレス・WSL2 Linux カーネル双方向検証・WinFsp 公式 `winfsp-tests`（45/45 PASS）整備およびドキュメント整合 |
| `v0.10.2` | 2026-11-28 | `--rw-fast-io` のデータ先行書込＆部分失敗ロールバック・R/W ガード対称化・`win32_io` 書込エラー時キャッシュ無条件破棄、`ZEROEXT4_FSX_SEED`/`ZEROEXT4_FSX_OPS` 可変ファザー・4 GiB スパース境界テスト（計 40 関数）・WinFsp 実マウント乱数 I/O + WSL2 検証拡充 |
| `v0.11.0` | 2026-11-28 | **Phase 1〜3 完了**: Overlay v2 識別子・非空未識別フォルダ拒否・`inspect` 拡張診断／`doctor`（GUI Dirty 時 RW 自動抑止含む）、`overlay status/diff/validate/export/reset`・O(1) 双方向ハッシュマップ＆決定論的 inode・WinFsp Reparse Point symlink・Windows 予約名／大小文字衝突ガード・ケースのみリネーム修正、未対応 ext4 機能明示検出＆`--ignore-csum-errors`・非破壊 4 パス診断 `fsck-lite`・既定 `--rw` ステートレス方針明確化・POSIX UID/GID/パーミッション設定（CLI/GUI・ブラウザ `UID:GID` 表示含む、CTest 54 関数・GUI 12 テスト）・性能評価レポート `v0.11.0` 全項目再計測 |
| `v0.11.1` | 2026-11-28 | ケース非区別環境での実ディレクトリ／自己参照 symlink 共存時のオープン不具合修正（`dentry_cache` 衝突ガード・Overlay `lower_ino` 分離・Copy-up 後実表記解決）、リリースアーカイブへ [利用ガイド](guides/usage.md) 同梱（CTest 57 関数・GUI 12 テスト） |

---

## 2. 基本方針と安全境界

`ZeroExt4` の中核価値は、以下の 4 点にある。

1. **BSOD リスクゼロのユーザー空間実装**: Windows カーネル空間へ独自の ext4 コードを入れず、`WinFsp` + 上流 `e2fsprogs` (`libext2fs`) により安全性を担保する。
2. **Native NTFS 並みの高速読取**: 2-Tier I/O バイパス（`FileInfoTimeout=INFINITE` による NT Cache Manager 協調＋`O(log N)` Direct Extent I/O＋`FILE_FLAG_OVERLAPPED` 並列非同期 I/O）により、WSL2 9P を大きく上回る性能を実現する。
3. **非破壊の NTFS Overlay 運用 (`--overlay`)**: Linux 側 ext4 ボリュームを厳密に Read-Only に保ったまま、変更・新規作成・Whiteout 削除だけを Windows NTFS 上の Upperdir へ逃がせる。
4. **診断・検証基盤の内蔵**: リアルタイム統計 IPC、Perfetto トレース (`--trace`)、ベンチマーク (`zeroext4_bench.exe`)、ASan / CRT リーク検出 / `/analyze` が既に整備されている。

したがって今後の拡張は、単なる機能追加よりも以下の**設計上の安全境界**を守りながら段階的に進める。

- **Direct Extent I/O は Lower Read-Only 限定を維持する**:
  - 現行どおり **Read-Only（既定 ON）** および **Overlay Lower 側（既定 ON）** に限定し、**`--rw` では既定無効・`--direct-io` 併用拒否** のポリシーを維持する。`libext2fs` の内部 dirty buffer・journal・inode cache が関与する直接書き込みモードでの物理セクタバイパスは、わずかな無効化遅延やキャッシュ不整合が重大なデータ破損・Read-After-Write 不整合に直結するため解禁しない。
- **Windows 上での破壊的 FS 自動修復（`e2fsck -y` 相当）は非目標とする**:
  - ext4 の修復は Linux 本家の `e2fsck` が最も安全かつ正確である。`ZeroExt4` は未再生ジャーナル／エラーフラグ／不整合の確実な検出と非破壊の事前診断（`inspect` / `fsck-lite`）に徹し、危険なボリュームに対する不用意な RW マウントを確実に阻止する。

---

## 3. 今後のフェーズ全体像

| Phase | テーマ | 主な対象・成果物 | 状況 / 優先度 |
| :--- | :--- | :--- | :---: |
| **Phase 1**<br>（短期） | **安全性・診断の強化** | 非空未識別 Upperdir の拒否＆v2 `.wh.__ext4_lower_id` 強化、GUI Dirty/エラー時 RW 抑止、`inspect` 診断拡充・`zeroext4 doctor`（GUI i18n は `v0.5.0`、安全 `unmount` 検証は `v0.6.0`、`--case-sensitive` は `v0.8.0` で完了） | **完了** |
| **Phase 2**<br>（中期） | **Overlay 運用性・スケーラビリティ・名前空間の強化** | `overlay status/diff/validate/export/reset`、ハッシュ化による inode/path マップ O(1) 高速化、WinFsp Reparse Point 経由の symlink 透過対応、Windows 予約名／大小文字衝突の整理 | **完了** |
| **Phase 3**<br>（中長期） | **ext4 互換性・読み取り専用診断・RW 標準経路の改善** | `fscrypt` / 外部ジャーナル等の明示検出＆`--ignore-csum-errors`、非破壊診断 `zeroext4 fsck-lite`、多段 extent / sparse / inline / csum 破損境界テスト、既定 `--rw` のステートレス `libext2fs` 経路維持＆`inline_data` 境界修正 | **完了** |
| **Phase 4**<br>（長期） | **配布パッケージング・エコシステム拡充** | `winget` マニフェスト、`.NET` 自己完結／ポータブル配布評価、停止済み WSL2 `ext4.vhdx` 連携ワークフロー強化 | **低〜中** |

---

## 4. Phase 1：安全性・診断の強化（完了）

### 4.1 Overlay Upperdir 安全性・Lower 識別子ガードの強化（完了）

`src/overlay/overlay_vfs.c` および WPF GUI（`DriveLetterService` / `MainViewModel`）で以下を実装済み：

- **非空かつマーカー無し Upperdir の拒否**:
  - `.wh.__ext4_lower_id` が存在しない非空 Upperdir は CLI・GUI とも安全のため拒否（`-ENOTEMPTY` / 事前バリデーションエラー）し、新しいフォルダまたは空フォルダの指定を促す。
- **Lower 識別子の v2 拡張（複製 `.img` / VHDX・破損対策）**:
  - `v2;uuid=<lower_uuid>;blocks=<blocks>;bsize=<bsize>;part_uuid=<part_uuid>;part_off=<part_off>;part_size=<part_size>;cs=<0|1>` 形式へ拡張し、GPT Partition UUID やパーティション配置を含めて照合する（既存の v1 形式も後方互換維持）。
  - 破損した `.wh.__ext4_lower_id` (`-EINVAL`) および未知の将来バージョン (`-ENOTSUP`) は安全のため Overlay RW を拒否する。
- **GUI 事前診断との連携**:
  - `NEEDS_RECOVERY` / `HAS_ERRORS` 等で `MountAdvice.ReadWriteAllowed == false` のボリュームでは、GUI 上で Read-Write カードを自動抑止し、保存済み RW プリセットも自動で Read-Only へフォールバックする。

### 4.2 `inspect` 診断出力の拡充と `zeroext4 doctor` コマンド（完了）

- **`zeroext4 inspect --json` / `list --json` の拡張**:
  - `partition_uuid`、可読な機能フラグ配列（`feature_compat_names` / `feature_incompat_names` / `feature_ro_compat_names`）、`last_mount_time` / `last_fsck_time` / `fsck_error_count`、`metadata_csum_enabled` / `sb_checksum_valid`、および `mount_advice`（`read_only_allowed` / `overlay_allowed` / `read_write_allowed` / `recommended_mode` / 理由）を出力する。
- **`zeroext4 doctor` サブコマンドの追加**:
  - WinFsp ランタイム導入状態（DLL パス・バージョン）、プロセス管理者権限状態、指定ソースの健全性、および指定 `--overlay` Upperdir 整合性を 1 コマンドで診断・JSON 出力できる自己診断コマンドを追加し、GUI「詳細 & ログ」タブからも呼び出し可能とした。

---

## 5. Phase 2：Overlay 運用性・スケーラビリティ・名前空間の強化（完了）

### 5.1 Overlay 差分管理コマンド & GUI 支援 (`overlay status / diff / validate / export / reset`)（完了）

- **`zeroext4 overlay status / diff / validate / export / reset --upper <dir> [--lower <src>] [--json]`**:
  - `status` / `diff`: Upperdir 内の変更済み（`modified`）・新規（`added`）・削除（`.wh.*` Whiteout）・不透過ディレクトリ（`.wh.__dir_opaque`）の件数・一覧・Upper 消費容量を集計し JSON / テキスト出力する。
  - `validate`: v2/v1 マーカー整合性、孤立 Whiteout、残留一時ファイル (`.ext4tmp_*`)、NTFS Reparse Point、Windows 非互換名・パス長を事前検査する。
  - `export`: `manifest.json`（差分メタデータ・Lower ID 情報）と変更ファイル群 (`files/...`) を POSIX USTAR/PAX `.tar` アーカイブとして書き出す。
  - `reset --yes`: `.wh.__ext4_lower_id` を保持したまま差分内容を安全に初期化する（マーカー無し非空フォルダは無条件拒否）。
- **GUI 上での差分プレビューと安全なリセット**:
  - `ZeroExt4Gui.exe` の Overlay カードに **「差分を確認」「エクスポート (.tar)」「差分をリセット」**（稼働中マウント時は抑止＋確認ダイアログ付き）と差分エントリプレビューを追加した。

### 5.2 Overlay 内部構造のスケーラビリティと inode 安定性（完了）

- **双方向ハッシュテーブルによる O(1) ルックアップ**:
  - `src/overlay/overlay_vfs.c` の inode ⇔ path マッピングを単方向リスト (`O(N)`) から動的リサイズ可能な `ino_buckets` + `path_buckets`（Shared ロック高速パス付き `O(1)`）へ置換し、`overlay_vfs_readdir()` 内の `ov_name_list_t` もハッシュ索引化した。
- **合成 inode 番号（`0x80000000U+`）の決定論的割り当て**:
  - パスハッシュ由来の決定論的合成 inode と Linear Probing により再マウント後も同一パスの合成 inode を維持し、`rename` 時も `ino` を不変に保つようにした。
- **Lower 大規模ディレクトリ `rename` 時の事前検証＆安全制御**:
  - 事前ツリー容量・件数走査 (`measure_lower_tree_copyup`)、NTFS 空き容量事前チェック (`-ENOSPC` 早期拒否)、大容量 Copy-up 警告、および失敗時の部分コピーロールバックを実装した（GUI / ドキュメントでも仕様を明記）。

### 5.3 WinFsp シンボリックリンク透過対応（Reparse Point）および名前空間エッジケース（完了）

- **Windows Reparse Point (`IO_REPARSE_TAG_SYMLINK`) 連携**:
  - `VolumeParams.ReparsePoints = 1` および `ResolveReparsePoints` / `GetReparsePoint` / `SetReparsePoint` / `DeleteReparsePoint` を実装し、ボリューム内相対・POSIX 絶対（ボリューム相対への変換）およびディレクトリ symlink（`ReadDirectory` ロック外後処理による `FILE_ATTRIBUTE_DIRECTORY` 付与・循環参照上限 16 段）に対応した。ホスト側脱出パス（ドライブ文字 / UNC / `..` 越境）は安全のため遮断する。
  - Overlay 時は Lower symlink 参照 (`readlink`) と削除 (`.wh.*`) を許可しつつ、Copy-up / 新規 symlink 作成 / リネームは `-ENOTSUP` として明確に禁止した。
- **Windows 固有の名前空間ガード & 大文字小文字衝突解決**:
  - `src/common/win_namespace.c` で予約デバイス名（`CON`, `NUL`, `AUX`, `PRN`, `COM1..9`, `LPT1..9` 拡張子付き含む）、禁止文字 (`< > : " | ? * \`)、末尾ドット／空白を検出し、WinFsp 作成／リネーム・Overlay Copy-up・Lower 無条件スキップ（Lower RO 閲覧は安全維持）を実装した。
  - 既定 Case-Insensitive モードにおける `Foo.txt` / `foo.txt` 衝突時は「厳密一致優先 → 辞書順最小名 → 最小 inode」の決定論的ルールと警告ログ＋`stat_ci_collisions` カウンタを導入した。

---

## 6. Phase 3：ext4 互換性・読み取り専用診断 (`fsck-lite`)・RW 標準経路の改善（完了）

### 6.1 未対応 ext4 機能の明示検出と境界テスト拡充（完了）

- **未対応／制限付き ext4 機能の明示検出とガード**:
  - `src/disk/superblock_probe.c` で `s_journal_dev`（外部ジャーナル参照 `ext4_probe_has_external_journal`）、`unsupported_incompat`、`unsupported_ro_compat` を検出し、`ext4_evaluate_mount_advice`・`ext4_vfs_mount`・`zeroext4 inspect` に理由と対処方法付きの詳細診断を実装した。
  - `EXT3_FEATURE_INCOMPAT_JOURNAL_DEV` および未知 `incompat` ビットは `-ENOTSUP` でマウントを拒否し、`fscrypt` (`encrypt`) / `fs-verity` (`verity`) / 外部ジャーナル (`s_journal_dev`) / 未知 `ro_compat` は直接 `--rw` を `-ENOTSUP` で遮断したうえで RO／Overlay 時に明示警告を出力する。
  - CLI `mount` に `--ignore-csum-errors` オプションを実装し、`--rw` との併用は `-EINVAL` で拒否して RO／Overlay の診断・救出用途に限定した。
- **ext4 フォーマット境界の回帰テスト拡充**:
  - `test_ext4_format_boundaries_phase3` 等により、1KB ブロック 380 断片による `eh_depth >= 2` 多段エクステント木、実ブロック非ゼロ汚染済みの未初期化エクステント (`EXT2_EXTENT_FLAGS_UNINIT`) の RO/RW 双方でのゼロ埋め読取、`inline_data` の RO Direct I/O フォールバック読取および `--rw` 書き込み時の透過的エクステント展開、スパース／`truncate`／`huge_file`（4 GiB 超境界）・`metadata_csum` 不正検出と `--ignore-csum-errors` を網羅した。

### 6.2 読み取り専用整合性診断 (`zeroext4 fsck-lite`)（完了）

- **非破壊診断エンジン & CLI／GUI 連携**:
  - [fsck_lite.h](../src/disk/fsck_lite.h)・[fsck_lite.c](../src/disk/fsck_lite.c) を新設し、常に `read_only = 1` ＋ `EXT2_FLAG_IGNORE_CSUM_ERRORS`（書き込みゼロ）で 4 パスの整合性検査を行う `zeroext4 fsck-lite <image_or_disk> [--partition <N>] [--json]` および `ZeroExt4CoreClient.RunFsckLiteAsync` を実装した。
- **主な検査項目**:
  - **Pass 1**: スーパーブロック magic・パーティション境界超過・CRC32C チェックサム、各ブロックグループ記述子 (BGD) のチェックサム・ビットマップ/inode table 境界・空き数上限検査
  - **Pass 2**: ブロック／inode ビットマップ読込およびメタデータチェックサム検証、重複ブロック追跡（64 GB 以下は厳密ビットセット、超大容量時は有界ハッシュテーブル）
  - **Pass 3**: 割り当て済み inode の CRC32C チェックサム、`EXT4_EXTENTS_FL` ツリーの論理オフセット単調性／重複／物理ブロック境界・ビットマップ整合・複数ファイル間の物理ブロック重複検出 (`EXTENT_DUPLICATE_BLOCK`)
  - **Pass 4**: 全ディレクトリのブロックチェックサム・`rec_len`/`name_len`/inode 範囲・`.`/`..`・htree/inline_data 整合、孤立 inode チェーン (`s_last_orphan`) の循環・範囲検査、JBD2 ジャーナルヘッダ (`magic`, `s_start`, `s_errno`) 検査

### 6.3 `--rw` 標準 `libext2fs` 経路の整合性維持・ポリシー明確化（完了）

- **標準 `--rw` は完全ステートレス経路を維持（設計決定）**:
  - 4 モードの役割と安全性境界を明確化するため、既定の `--rw` は Direct Extent I/O を無効としたまま **I/O ごとに `ext2fs_file_open` / `ext2fs_file_close` を行う完全ステートレスな上流 `libext2fs` リファレンス経路** を維持する方針を決定した（キャッシュ性能は `win32_io` 32 シャードブロックキャッシュおよび `libext2fs` inode キャッシュで確保し、高速化パスは実験的オプトイン `--rw --rw-fast-io` に明確分離）。
  - また `ext4_vfs_standard_read_fallback_locked()` における `inline_data` 読み取り時の `EXT2_I_SIZE` 上限クランプ修正（上流 `ext2fs_file_read_inline_data` の余剰ゼロ埋め回避）と、標準 `--rw` 連続書込／別ハンドル間即時可視性／`truncate` 後 `fsck-lite` 整合性検証（`test_rw_standard_stateless_coherence`）を追加した。

---

## 7. Phase 4：配布・パッケージング・エコシステム拡充

### 7.1 パッケージ配布 (`winget` / ポータブル ZIP / 自己完結ビルド)

- GitHub Actions の `v*.*.*` リリース自動公開ワークフロー（[.github/workflows/release.yml](../.github/workflows/release.yml)）を基盤に、`winget` 公式パッケージ登録（`winget-pkgs`）を検討する。
- 現在 `ZeroExt4Gui.exe` はフレームワーク依存の `.NET 10 Desktop Runtime` 前提となっているため、ランタイム未導入環境でもそのまま起動できる自己完結（`self-contained`）配布版の提供要否・サイズトレードオフを評価する。

### 7.2 WSL2 `ext4.vhdx` 連携ワークフロー強化

- 停止済み WSL2 仮想ディスク（`ext4.vhdx`）の安全な Read-Only 検査・高速参照手順について、WSL 稼働中の二重アクセス衝突を防ぐプリチェックや GUI/CLI 補助ガイドを拡充する。
