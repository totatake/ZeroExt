# 今後の構想・ロードマップ (`ZeroExt` / `zeroext`)

本書は、現行の `ZeroExt`（`v0.4.x` 時点：`zeroext.exe` ユーザー空間ドライバ／CLI ＆ `ZeroExtGui.exe` デスクトップアプリ）について、今後の安全強化・国際化・運用性向上・ext4 互換性拡充のロードマップを定義するものである。

> [!NOTE]
> 当初構想していた **Phase 0 基盤（GUI アプリケーション `ZeroExtGui.exe` および Perfetto トレース出力 `--trace`）は実装完了済み** である。概要と関連設計は末尾の [完了済み基盤](#完了済み基盤implemented-foundations) を参照されたい。

---

## 1. 基本方針と安全境界

`ZeroExt` の中核価値は、以下の 4 点にある。

1. **BSOD リスクゼロのユーザー空間実装**: Windows カーネル空間へ独自の ext4 コードを入れず、`WinFsp` + 上流 `e2fsprogs` (`libext2fs`) により安全性を担保する。
2. **Native NTFS 並みの高速読取**: 2-Tier I/O バイパス（`FileInfoTimeout=INFINITE` による NT Cache Manager 協調＋`O(log N)` Direct Extent I/O＋`FILE_FLAG_OVERLAPPED` 並列非同期 I/O）により、WSL2 9P を大きく上回る性能を実現する。
3. **非破壊の NTFS Overlay 運用 (`--overlay`)**: Linux 側 ext4 ボリュームを厳密に Read-Only に保ったまま、変更・新規作成・Whiteout 削除だけを Windows NTFS 上の Upperdir へ逃がせる。
4. **診断・検証基盤の内蔵**: リアルタイム統計 IPC、Perfetto トレース (`--trace`)、ベンチマーク (`zeroext_bench.exe`)、ASan / CRT リーク検出 / `/analyze` が既に整備されている。

したがって今後の拡張は、単なる機能追加よりも以下の**設計上の安全境界**を守りながら段階的に進める。

- **Direct Extent I/O は Lower Read-Only 限定を維持する**:
  - 現行どおり **Read-Only（既定 ON）** および **Overlay Lower 側（既定 ON）** に限定し、**`--rw` では既定無効・`--direct-io` 併用拒否** のポリシーを維持する。`libext2fs` の内部 dirty buffer・journal・inode cache が関与する直接書き込みモードでの物理セクタバイパスは、わずかな無効化遅延やキャッシュ不整合が重大なデータ破損・Read-After-Write 不整合に直結するため解禁しない。
- **Windows 上での破壊的 FS 自動修復（`e2fsck -y` 相当）は非目標とする**:
  - ext4 の修復は Linux 本家の `e2fsck` が最も安全かつ正確である。`ZeroExt` は未再生ジャーナル／エラーフラグ／不整合の確実な検出と非破壊の事前診断（`inspect` / `fsck-lite`）に徹し、危険なボリュームに対する不用意な RW マウントを確実に阻止する。

---

## 2. ロードマップ全体像

| Phase | テーマ | 主な対象・成果物 | 優先度 |
| :--- | :--- | :--- | :---: |
| **Phase 1**<br>（最優先・短期） | **GUI 多言語化（英語 / 日本語）と安全性・診断の強化** | `ZeroExtGui.exe` i18n（英語／日本語切替）、非空レガシー Upperdir の既定拒否＆Lower ID 強化、`inspect` 診断情報拡充・`zeroext doctor` | **高** |
| **Phase 2**<br>（中期） | **Overlay 運用性・スケーラビリティ・名前空間の強化** | `overlay status/diff/validate/export`、ハッシュ化による inode/path マップ高速化、WinFsp Reparse Point 経由の symlink 透過対応、Windows 予約名／大小文字衝突の整理 | **中〜高** |
| **Phase 3**<br>（中長期） | **ext4 互換性・読み取り専用診断・RW 標準経路の改善** | `fscrypt` / 外部ジャーナル等の明示検出、非破壊診断 `zeroext fsck-lite`、多段 extent / sparse / inline / csum 破損境界テスト、`--rw` 標準 `libext2fs` 経路の最適化 | **中** |
| **Phase 4**<br>（長期） | **配布パッケージング・エコシステム拡充** | `winget` マニフェスト、`.NET` 自己完結／ポータブル配布評価、停止済み WSL2 `ext4.vhdx` 連携ワークフロー強化 | **低〜中** |

---

## 3. Phase 1：GUI 多言語化（英語 / 日本語）と安全性・診断の強化

### 3.1 GUI アプリケーションの多言語化（i18n：英語 / 日本語）

現在 GitHub 公開に合わせて `README.md` は英語化された一方、`gui/ZeroExt.Gui/` 配下は画面文言・ツールチップ・ステータス表示・エラー翻訳・システムトレイメニューがすべて日本語ハードコードとなっている（対象：`Views/MainWindow.xaml`、`Views/CreateImageDialog.xaml`、`Views/ForceUnmountDialog.xaml`、`Views/WinFspGuideDialog.xaml`、`ViewModels/MainViewModel.cs`、`Services/MountErrorTranslator.cs`、`Services/TrayIconService.cs`、`Converters/Converters.cs`）。海外ユーザーも含め直感的に利用できるよう、GUI の多言語化を最優先で実施する。

- **UI 文字列の外出しとリソース化**:
  - `.resx` または XAML `ResourceDictionary` キー参照方式を採用し、全画面 XAML・ダイアログ・ViewModel ステータス文字列・トレイコンテキストメニューを一元管理する。
  - `MountErrorTranslator.cs` が返すタイトル・メッセージ・対処提案文言も言語リソースから解決するよう改修する。
- **言語設定と自動選択**:
  - `GuiSettings`（`gui/ZeroExt.Gui/Models/GuiSettings.cs`）に `Language` プロパティ（`"auto"` / `"en"` / `"ja"`）を追加する。
  - 既定値 `"auto"` では OS ロケール（`CultureInfo.CurrentUICulture`）を参照し、日本語環境は `"ja"`、それ以外の環境は `"en"` へフォールバックする。
  - GUI ヘッダー／設定欄からアプリ再起動なし（または即時リソース差し替え）で **English / 日本語** を切り替えられるようにする。
- **自動テスト (`ZeroExt.Gui.Tests`)**:
  - `en` / `ja` リソース間で全キーが過不足なく定義されていることを検証するキー網羅テスト、および主要エラーに対する `MountErrorTranslator` の日英切替出力テストを追加する。

### 3.2 Overlay Upperdir 安全性・Lower 識別子ガードの強化

現在 `src/overlay/overlay_vfs.c`（`overlay_validate_lower_id`）は Upperdir 直下の `.wh.__ext4_lower_id` に `<lower_uuid>:<blocks_count>:<block_size>` を保存し、別ボリュームとの不一致時はエラーを返している。残る安全ギャップを以下の通り解消する。

- **非空かつマーカー無し Upperdir の既定拒否**:
  - 現状は Upperdir に既存ファイルがあるにも関わらず `.wh.__ext4_lower_id` が存在しない場合、警告ログ（`EXT4_LOG_WARN`）を出してそのままマウント＋新規マーカー書き込みへ進むため、関係のない既存フォルダ誤指定時に Lower ファイルを意図せず上書きシャドウイングするリスクがある。
  - 今後は **「非空かつ `.wh.__ext4_lower_id` が無い Upperdir」を既定で拒否** し、CLI では明示フラグ（例：`--allow-legacy-upper`）、GUI では確認ダイアログ経由でのみ許可する安全側に倒す。
- **Lower 識別子の拡張（複製 `.img` / VHDX・破損対策）**:
  - 同一 ext4 UUID を持つクローン `.img` やコピー済み VHDX を誤って同一とみなさないよう、後方互換なバージョン付き形式へ拡張し、GPT Partition UUID・パーティション開始セクタ／サイズ・Overlay 形式バージョン等を補助識別情報として保持・照合できるようにする。
  - `.wh.__ext4_lower_id` ファイルの破損や未知の将来フォーマットを検知した場合は安全のため Overlay RW を拒否し、明確な理由を通知する。
- **GUI 事前診断との連携**:
  - 選択されたボリュームが `NEEDS_RECOVERY` / `HAS_ERRORS` の場合、GUI 上で Read-Write 選択を自動抑止して Read-Only または Overlay（Lower RO 保持）を推奨表示する。
  - マウント開始前に選択済み Upperdir と対象 Lower の整合性を事前チェックし、不一致時は即座に警告・別 Upperdir 指定を促す。

### 3.3 `inspect` 診断出力の拡充と `zeroext doctor` コマンド

- **`zeroext inspect --json` の拡張**:
  - 現在の数値ビットフラグ（`feature_compat` / `feature_incompat` / `feature_ro_compat` / `journal_state` / `journal_start` 等）に加え、可読な機能フラグ名配列、Partition UUID・開始セクタ・セクタ数、`last_mount_time` / `last_fsck_time` / `fsck_error_count`、チェックサム状態、および推奨マウント可否サマリー（`mount_advice`: Read-Only / Overlay / Read-Write 可否と理由）を出力する。
- **`zeroext doctor` サブコマンドの追加**:
  - WinFsp ランタイム導入状態、プロセス管理者権限状態、および指定ソース（`--source` / `--partition`）のマウント可否を 1 コマンドで診断・JSON 出力できる自己診断コマンドを追加する。

---

## 4. Phase 2：Overlay 運用性・スケーラビリティ・名前空間の強化

### 4.1 Overlay 差分管理コマンド & GUI 支援 (`overlay status / diff / validate / export`)

`--overlay` の大きな強みは「Lower ext4 を傷つけず、NTFS Upperdir に差分だけが残る」点にある。これを運用ワークフローとして完結させるため、CLI サブコマンド群と GUI 連携を追加する。

- **`zeroext overlay status / diff --upper <dir> [--lower <src>] [--json]`**:
  - Upperdir 内の変更済みファイル（Copy-up 由来）・新規作成ファイル・削除（`.wh.*` Whiteout）・不透過ディレクトリ（`.wh.__dir_opaque`）の件数と一覧・Upper 消費容量を表示する。
- **`zeroext overlay validate --upper <dir> --lower <src> [--partition N]`**:
  - Lower ID マーカー整合性、孤立した Whiteout／不正パスの有無を事前検査する。
- **`zeroext overlay export --upper <dir> --output <archive.tar>`**:
  - Upper 上の差分に加え、元の ext4 パス・Lower UUID・Whiteout / Opaque 情報・POSIX mode 等をマニフェスト付きアーカイブ（`.tar` + JSON マニフェスト等）として書き出し、バックアップや Linux 側への安全な反映検討に利用できるようにする。
- **GUI 上での差分プレビューと安全なリセット**:
  - 指定 Upperdir 内の変更件数やサイズをマウント前後で確認し、不要になった差分フォルダの安全な初期化を行える UI を提供する。

### 4.2 Overlay 内部構造のスケーラビリティと inode 安定性

- **`ov_ino_entry_t` の O(N) リニア探索からハッシュテーブル化への置換**:
  - 現行 `src/overlay/overlay_vfs.c` の双方向 inode ⇔ path マッピング（`ov_ino_entry_t *entries`）は単方向連結リストの線形探索となっており、数万ファイル規模のディレクトリ走査・並列アクセス時に `map_lock` 競合および検索コストが増大する。Open-addressing またはバケットハッシュ＋パスインデックスへ置き換え、`O(1)` 化する。
- **合成 inode 番号（`0x80000000U+`）の再現性と衝突対策**:
  - マウントセッションを跨いだ際のパスベース合成 inode の安定化・Lower inode との衝突回避ルールを整理し、長期キャッシュやエクスプローラー更新との親和性を高める。
- **Lower 大規模ディレクトリ `rename` 時のコスト対策**:
  - 現行の `overlay_vfs_rename()` は Lower 側ディレクトリをリネームする際、`copy_lower_tree_to_upper()` によりサブツリー全体を NTFS Upperdir へ再帰 Copy-up する仕様となっている。巨大モデルやデータセットを含むディレクトリで意図せず大量コピーが発生するリスクをドキュメント・GUI 警告で明示しつつ、容量事前チェックや中断安全性の向上を検討する。

### 4.3 WinFsp シンボリックリンク透過対応（Reparse Point）および名前空間エッジケース

- **Windows Reparse Point (`SYMLINK`) 連携**:
  - VFS 層には既に `src/vfs/op_symlink.c`（`ext4_vfs_readlink` / `ext4_vfs_symlink`）が存在する。`src/driver/winfsp_driver.c` 側で `GetReparsePoint` 等を実装し、少なくとも Read-Only マウントにおいて ext4 上のシンボリックリンクを Windows 側へ安全に公開できるようにする（ディレクトリ循環参照防止やセキュリティ境界を明確化）。
  - Overlay におけるシンボリックリンク／ハードリンク挙動（Lower symlink 参照・Upper 側作成ルールと制限事項）の仕様確定と回帰テスト追加を行う。
- **Windows 固有の名前空間エッジケース対策**:
  - `CON`, `AUX`, `NUL`, `COM1` 等の Windows 予約デバイス名や `:` / `*` / `?` 等を含む ext4 パスを安全に扱うためのガードと変換／アクセス抑止ルールを整理する。
  - ext4 の大文字小文字区別（case-sensitive）ディレクトリ上で同一ディレクトリ内に `Foo.txt` と `foo.txt` が共存する場合の探索・解決優先順位をテストケース付きで明確化する。

---

## 5. Phase 3：ext4 互換性・読み取り専用診断 (`fsck-lite`)・RW 標準経路の改善

### 5.1 未対応 ext4 機能の明示検出と境界テスト拡充

- **未対応 incompatible 機能の明示エラー化**:
  - `fscrypt`（`EXT4_FEATURE_INCOMPAT_ENCRYPT`）、`fs-verity`、外部ジャーナルデバイス等の未サポート機能が有効なボリュームをプローブ／マウントした際、汎用エラーではなく理由と対処方法を明示したエラー・警告を返すようにする。
- **ext4 フォーマット境界の回帰テスト拡充**:
  - 多段（depth >= 2）エクステントツリー、uninitialized extent（`EXT2_EXTENT_FLAGS_UNINIT`）、スパースファイル（hole ゼロ埋め・`truncate` 境界）、`inline_data`、`huge_file` / 64bit ブロック、`metadata_csum` 不正時のエラー検出・`--ignore-csum-errors` 挙動をカバーするテストを拡充する。

### 5.2 読み取り専用整合性診断 (`zeroext fsck-lite`)

- Windows 上で ext4 メタデータの健全性を非破壊で検証できる `zeroext fsck-lite <image_or_disk> [--partition N] [--json]` を実装する。
- 主な検証対象：
  - スーパーブロック／ブロックグループ記述子（magic, block/inode size, 64bit フィールド、checksum）
  - ブロック／inode ビットマップおよび inode table のパーティション境界内チェック
  - エクステントツリーの越境・論理オフセット不整合・重複ブロック候補検出
  - ディレクトリエントリの破損・不正長、孤立 inode リスト、JBD2 ジャーナルヘッダ状態
- **位置づけ**: 危険検出とレポート出力に限定し、修復操作は一切行わない。

### 5.3 `--rw` 標準 `libext2fs` 経路の最適化（Direct I/O は無効維持）

- 前述の安全境界通り、`--rw` では Direct Extent I/O を有効化せず、整合性を最優先する。
- 直接 `--rw` マウント時の書き込み・読み取りスループット改善が必要な場合は、以下の**標準経路内の安全な最適化**で対応する：
  - オープン済みハンドル (`ext4_file_handle_t`) における `ext2_file_t` の再利用（毎 Read/Write での `ext2fs_file_open` / `close` 削減）
  - 連続小口書き込みのバッチ化と `ctx->rwlock` 排他ロック保持時間の短縮
  - メタデータ更新時（`truncate` / `unlink` / `rename` / `write`）の各種キャッシュ無効化がすべて排他ロック保護下で行われることの継続検証

---

## 6. Phase 4：配布・パッケージング・エコシステム拡充

### 6.1 パッケージ配布 (`winget` / ポータブル ZIP / 自己完結ビルド)

- 既に稼働している GitHub Actions の `v*.*.*` リリース自動公開 CI（[.github/workflows/ci.yml](../.github/workflows/ci.yml)）を基盤に、`winget` 公式パッケージ登録（`winget-pkgs`）を検討する。
- 現在 `ZeroExtGui.exe` はフレームワーク依存の `.NET 10 Desktop Runtime` 前提となっているため、ランタイム未導入環境でもそのまま起動できる自己完結（`self-contained`）配布版の提供要否・サイズトレードオフを評価する。

### 6.2 WSL2 `ext4.vhdx` 連携ワークフロー強化

- 停止済み WSL2 仮想ディスク（`ext4.vhdx`）の安全な Read-Only 検査・高速参照手順について、WSL 稼働中の二重アクセス衝突を防ぐプリチェックや GUI/CLI 補助ガイドを拡充する。

---

## 完了済み基盤（Implemented Foundations）

以下は初期構想（旧 Phase 0）から既に実装・統合され、実運用および自動テストに組み込まれている基盤機能である。

### A. デスクトップ GUI アプリケーション (`ZeroExtGui.exe`) — **実装済み**

- **実装場所**: `gui/ZeroExt.Gui/`（WPF / `.NET 10`）、テスト：`gui/ZeroExt.Gui.Tests/`（9 件自動テスト + `--self-test`）。
- **実現済み機能**:
  - 物理ディスク・`.img` ファイル（ドラッグ＆ドロップ対応）の自動一覧と GPT / MBR / EBR・ラベル・UUID・使用率・JBD2 ジャーナル状態（`CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`）可視化
  - Read-Only（既定・Direct I/O ON）／Overlay（`.wh.__ext4_lower_id` 検証付き）／Read-Write（Dirty 時自動 RO フォールバック）の切替、詳細オプション、CLI プレビュー
  - リアルタイム転送統計・システムトレイ常駐・ワンクリック安全アンマウント・残留ドライブ解除・WinFsp 導入ガイド・非管理者時の UAC 再起動導線・WinFsp 不要のオフラインファイルブラウザ／`mkfs`
- **アーキテクチャ境界**: [アーキテクチャ設計書 ADR-002](architecture.md#adr-002-独立プロジェクト-zeroext-として分離開発する) に沿い、GPLv2 の `zeroext_core` / `libext2fs` を直接リンクせず `zeroext.exe` を `--json` / `--json-events` / `--stop-event` によるサブプロセス疎結合で制御する構成を維持している。詳細は [テスト報告書・利用ガイド](testing_and_usage.md) を参照。

### B. 性能解析用 Perfetto トレース出力機能 (`--trace`) — **実装済み**

- **実装場所**: `src/trace/perfetto_trace.h` / `src/trace/perfetto_trace.c`（テスト：`tests/test_perfetto_trace.c`）。
- **実現済み機能**:
  - 非指定時は単一分岐によるゼロオーバーヘッドを維持し、`zeroext mount --trace <path>`・`zeroext_bench.exe --trace <path>`・GUI 詳細設定から有効化可能
  - スレッドローカルリングバッファ（ロックレス記録・`QueryPerformanceCounter` 精度）へ WinFsp コールバック、`SRWLOCK` 待ち／保持時間、Extent／LRU キャッシュ、Direct I/O、`ReadFile`/`WriteFile`、Overlay Copy-up/Whiteout を記録し Chrome Trace Event JSON を出力（[Perfetto UI](https://ui.perfetto.dev/) 対応）
- **活用実績**: 実測トレースに基づく定量ボトルネック分析と Step 1〜4 最適化（最大 +153%〜1,294% 改善）については [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md) および [性能評価報告書](performance_evaluation.md) を参照。
