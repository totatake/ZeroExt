# Architecture Decision Records (ADR)

本ディレクトリは **ZeroExt** における主要なアーキテクチャ設計判断、検討した代替案、根拠、およびトレードオフを記録する ADR 一覧です。システム全体図とモジュール関係は [システムアーキテクチャ設計書](../architecture.md) を参照してください。

## ADR 一覧

| ID | タイトル | ステータス | 日付 | ドキュメント |
| :--- | :--- | :---: | :---: | :--- |
| `ADR-0001` | 純粋カーネルドライバ (`.sys`) ではなく `WinFsp + libext2fs` を採用する | Accepted | 2026-11-17 | [0001-winfsp-and-libext2fs-userspace-driver.md](0001-winfsp-and-libext2fs-userspace-driver.md) |
| `ADR-0002` | 独立プロジェクト (`ZeroExt`) として分離開発する | Accepted | 2026-11-17 | [0002-standalone-project-boundary.md](0002-standalone-project-boundary.md) |
| `ADR-0003` | `lwext4` ではなく本家 `e2fsprogs` (`libext2fs`) を採用する | Accepted | 2026-11-17 | [0003-upstream-libext2fs-over-lwext4.md](0003-upstream-libext2fs-over-lwext4.md) |
| `ADR-0004` | `fuse-ext2` 型モジュラー構造 × 64bit API × WinFsp ネイティブのハイブリッド設計 | Accepted | 2026-11-17 | [0004-modular-vfs-64bit-api-and-native-winfsp.md](0004-modular-vfs-64bit-api-and-native-winfsp.md) |
| `ADR-0005` | `e2fsprogs` 追従性を担保する Zero-Patch 互換レイヤー設計 | Accepted | 2026-11-17 | [0005-zero-patch-e2fsprogs-compat-layer.md](0005-zero-patch-e2fsprogs-compat-layer.md) |
| `ADR-0006` | カスタム `struct_io_manager` (`win32_io_manager`) 設計 | Accepted | 2026-11-17 | [0006-custom-win32-io-manager.md](0006-custom-win32-io-manager.md) |
| `ADR-0007` | 2 階層 `SRWLOCK` によるマルチスレッド同期設計 | Accepted | 2026-11-17 | [0007-two-tier-srwlock-concurrency.md](0007-two-tier-srwlock-concurrency.md) |
| `ADR-0008` | Dirty Journal 検出時の安全機構（RW 拒否 / RO フォールバック） | Accepted | 2026-11-17 | [0008-dirty-journal-safety-guard.md](0008-dirty-journal-safety-guard.md) |
| `ADR-0009` | 管理者権限実行時のドライブレター可視性（Mount Manager 優先 + デスクトップユーザーセッションへのフォールバック） | Accepted | 2026-11-17 | [0009-elevated-drive-letter-visibility.md](0009-elevated-drive-letter-visibility.md) |
| `ADR-0010` | WinFsp ABI 構造体レイアウトの検証とコンパイル時固定 | Accepted | 2026-11-17 | [0010-winfsp-abi-layout-assertions.md](0010-winfsp-abi-layout-assertions.md) |
| `ADR-0011` | `UmFileContextIsUserContext2 = 1` によるハンドル単位コンテキスト管理 | Accepted | 2026-11-18 | [0011-handle-context-via-user-context2.md](0011-handle-context-via-user-context2.md) |
| `ADR-0012` | RO / Overlay 既定有効・RW 自動無効のロックレス・エクステント直結 I/O | Accepted | 2026-11-18 | [0012-lockless-direct-extent-io-ro-overlay-default.md](0012-lockless-direct-extent-io-ro-overlay-default.md) |
| `ADR-0013` | NTFS を Upper 層とする非破壊 Overlay マウント と Lower-ID 照合 | Accepted | 2026-11-18 | [0013-ntfs-overlay-mount-and-lower-id-guard.md](0013-ntfs-overlay-mount-and-lower-id-guard.md) |

## 新しい ADR を追加する手順

1. 次の 4 桁連番で `NNNN-<short-kebab-title>.md` を本ディレクトリへ作成します。
2. 背景（Context）・検討した選択肢（Alternatives Considered）・決定（Decision）・結果と影響（Consequences）を記載します。
3. 本索引表および [../architecture.md](../architecture.md)（および関連設計書）へリンクを追記します。
