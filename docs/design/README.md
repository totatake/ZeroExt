# 詳細設計ドキュメント

本ディレクトリは **ZeroExt4** の内部モジュール設計・データ構造・WinFsp インターフェースおよびサブシステム設計パックを集約します。

## 設計ドキュメント一覧

| ドキュメント | 対象・概要 |
| :--- | :--- |
| [詳細設計書](detailed-design.md) | 全体コンポーネント設計、CMake 構成、互換レイヤー (`compat/`)、`win32_io`、ディスク検査、VFS (`op_*`)、Direct Extent / Overlay、WinFsp インターフェース |

> 大規模な新サブシステムや機能追加を行う際は、`docs/design/<feature>/` 配下に設計パック（`README.md`, `00-background.md`..`04-results.md`）を追加して本ファイルからリンクしてください。
