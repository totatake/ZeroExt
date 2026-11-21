# 今後の構想・ロードマップ (`ZeroExt` / `zeroext`)

本書は、現行の CLI ＆ WinFsp ユーザー空間ドライバ（`zeroext.exe`）の将来的な拡張構想として計画している **GUI アプリケーションの追加** および **性能解析用 Perfetto トレース出力機能の追加** について、目的・設計方針・想定機能を記録したものである。

---

## 1. GUI アプリケーションの追加（実装済み）

> [!NOTE]
> 本項目は `gui/ZeroExt.Gui/`（WPF / `.NET 10`、`ZeroExtGui.exe`）として実装されました。`zeroext.exe` とは `--json` / `--json-events` / `--stop-event` によるサブプロセス疎結合（GPLv2 境界維持）で連携します。詳細は [テスト報告書・利用ガイド](testing_and_usage.md) を参照してください。

### 1.1 背景と目的
現状の `zeroext.exe` はコマンドライン操作を前提としており、物理ディスク（`\\.\PhysicalDrive0..15`）の認識、パーティションや JBD2 ジャーナル状態の検査、マウントオプションの組み合わせ（Read-Only + Direct I/O / `--overlay` / `--rw` / `--no-direct-io`）、アンマウントおよび UAC 昇格制御をターミナルから手動で指定する必要がある。

一般的な Windows デスクトップ運用における視認性と操作性を高めるため、GUI フロントエンド（マウントマネージャー兼ステータスビューア）の追加を構想する。

### 1.2 想定機能
- **ディスク／イメージ一覧表示＆FS検査ビュー**:
  - `zeroext list` / `inspect` 相当の情報を一覧表示し、GPT / MBR / EBR パーティション構成、ボリュームラベル、UUID、ext2 / ext3 / ext4 種別、JBD2 ジャーナル状態（`CLEAN` / `NEEDS_RECOVERY` / `HAS_ERRORS`）を視覚的に確認可能にする。
- **マウント設定・起動制御**:
  - ソース（物理ディスクの特定パーティション、または `.img` ファイル）とマウント先（ドライブレター `X:` または NTFS 空フォルダ）を選択し、マウントモードを選択できる UI を提供する。
  - **モード選択**:
    - 既定: **Read-Only**（安全重視・Direct I/O 既定 ON）
    - **Overlay モード**（NTFS Upperdir フォルダ指定・Lower RO 保持＋Direct I/O 既定 ON）
    - **Read-Write モード**（`--rw`、Dirty 時ガード・RO フォールバック設定）
  - **詳細オプション**: `--no-direct-io`（RO / Overlay 時に標準ブロックキャッシュ経路へ切替）、`--cache-blocks`、UAC 管理者昇格が必要な物理ディスクに対する昇格起動フロー。
- **常駐・マウント監視（システムトレイ対応）**:
  - アクティブなマウント一覧、使用中ドライブレター／マウント先パス、簡易読み書き統計の表示およびワンクリック・安全アンマウント操作。
  - `--overlay` 使用時の Upperdir パス確認および `.wh.__ext4_lower_id` 不一致時のエラーメッセージ表示。

### 1.3 アーキテクチャ方針（疎結合の維持）
- [アーキテクチャ設計書 ADR-002](architecture.md#adr-002-独立プロジェクト-zeroext-として分離開発する) に沿い、GPLv2 の `zeroext_core` / `libext2fs` を GUI バイナリへ直接静的リンクすることは原則避け、**GUI プロセスとコアエンジン（`zeroext.exe`）をサブプロセス起動・標準入出力（将来的な構造化 JSON 出力等）または IPC で疎結合に分離する** 構成を基本とする。
- これにより、GUI 側の技術スタック（例: WinUI 3 / WPF / Win32 等）の自由度・ライセンス境界・クラッシュ分離性を維持できる。

---

## 2. 性能解析用 Perfetto トレース出力機能の追加（実装済み）

> [!NOTE]
> 本項目は `src/trace/perfetto_trace.h` / `src/trace/perfetto_trace.c` として実装されました。`zeroext mount --trace <path>`、`zeroext_bench.exe --trace <path>`、および `ZeroExtGui.exe` の詳細設定から有効化でき、生成された Chrome Trace Event JSON を [Perfetto UI](https://ui.perfetto.dev/) でそのまま可視化できます。詳細は [テスト報告書・利用ガイド](testing_and_usage.md)、実測トレースに基づくボトルネック解析・改善計画は [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md) を参照してください。

### 2.1 背景と目的
[性能評価報告書](performance_evaluation.md) で確認した通り、実マウント時のスループットやマルチスレッド並列アクセス時の挙動には、以下の複合要因が関与する。
- WinFsp カーネル〜ユーザー空間間の IRP ディスパッチ遅延とコールバック競合
- 2 階層 `SRWLOCK`（`ctx->rwlock` の Shared/Exclusive 競合、および `win32_io` の `cache_lock` 待ち）
- `extent_cache` ヒット有無および 8 MB ブロック LRU キャッシュヒット有無の境界
- `--direct-io`（`win32_io_pread_direct`）と標準 `ext2fs_file_read` 経路、および `--overlay` 時 Upper NTFS 側 Copy-up 処理の所要時間内訳

ログ出力やマクロベンチマークの総和だけでは各スレッド間の時系列上のロック待ち・I/O ブロッキング・キャッシュヒット率の相関を把握しづらいため、[Perfetto UI](https://ui.perfetto.dev/) で可視化できるタイムライントレース出力機能の追加を構想する。

### 2.2 計測対象イベント・カウンター（想定）
- **WinFsp コールバック境界スパン（スレッド別タイムライン）**:
  - `FspOpen`, `FspCreate`, `FspRead`, `FspWrite`, `FspReadDirectory`, `FspGetFileInfo`, `FspCleanup`, `FspClose` の開始・終了および転送バイト数・ステータス。
- **ロック競合・同期イベント**:
  - `ctx->rwlock`（Shared / Exclusive）および `win32_io` `cache_lock` の取得待ち時間と保持時間。
- **VFS / Overlay / Extent / I/O 層スパン＆カウンター**:
  - `extent_cache` ヒット／ミスおよびエクステント抽出時間（`ext4_vfs_get_extents` 等）
  - ブロック LRU キャッシュのヒット数・ミス数・RMW（Read-Modify-Write）発生回数カウンター
  - `win32_io_pread_direct` vs `ReadFile` / `WriteFile` の Win32 API 呼び出しレイテンシ
  - `--overlay` における Upper 解決・Copy-up（Lower → Upper コピー）および Whiteout 処理のスパン

### 2.3 設計方針
- **明示的有効化とゼロオーバーヘッド**:
  - CLI オプション（例: `zeroext mount ... --trace <output_path>` 等）を明示指定した場合のみトレース記録を有効化し、通常運用時はビルドフラグまたは低コスト分岐により性能劣化を生じさせない。
- **出力形式と低競合バッファリング**:
  - Perfetto UI（`ui.perfetto.dev`）でそのまま開けるトレース形式（Chrome Trace Event JSON 互換フォーマット、または軽量 protobuf TrackEvent 形式）を採用する。
  - トレース記録自体がロック競合を起こして観測対象を歪めないよう、スレッドローカルリングバッファ（`QueryPerformanceCounter` による高精度タイムスタンプ）へロックレス記録し、アンマウント時または定期フラッシュで出力する方式を優先検討する。
- **軽量な依存関係**:
  - C11 + MSVC ネイティブビルドの自己完結性（要件 `NFR-04`）を損なわないよう、巨大な C++ ランタイムや外部 SDK への強制依存を避けた構成を優先する。
