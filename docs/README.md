# ドキュメント一覧

本ディレクトリは **ZeroExt4**（`libext2fs` + `WinFsp` による Windows 向け高速ユーザー空間 ext2/ext3/ext4 ドライバ・CLI・WPF GUI）の技術ドキュメントを集約するハブです。

## 基本ドキュメント

| ドキュメント | 内容 |
| :--- | :--- |
| [技術概要・他手法比較・ユースケース](overview.md) | 開発背景、2-Tier I/O バイパス、他手法比較（NTFS / WSL2 9P 等）、実践ユースケース |
| [要件定義書](requirements.md) | 対応スコープ、ライセンス境界、機能要件 (`REQ-F-001`..`011`)・非機能要件 (`REQ-NF-001`..`004`) |
| [アーキテクチャ設計書](architecture.md) | システム構成図・I/O シーケンス・`e2fsprogs` 追従方針・ADR 索引 |
| [テスト・検証ガイド](testing.md) | CTest（6 スイート 32 関数）・GUI 自動テスト（11 件）・ASan / CRT リーク / `/analyze` |
| [ロードマップ](roadmap.md) | 完了済みマイルストーン（`v0.1.0`〜`v0.9.0`）、安全境界、Phase 1〜4 計画 |

## サブディレクトリ

| ディレクトリ / ハブ | 内容 |
| :--- | :--- |
| [ガイド一覧](guides/README.md) | [利用ガイド](guides/usage.md) / [開発・ビルド・保守ガイド](guides/development.md) |
| [Architecture Decision Records (ADR)](adr/README.md) | `ADR-0001`〜`ADR-0013`（WinFsp 採用・ライセンス分離・ロック階層・Direct I/O・Overlay 等） |
| [詳細設計ドキュメント](design/README.md) | [詳細設計書](design/detailed-design.md) および将来のサブシステム設計パック |
| [評価・解析レポート](reports/README.md) | [性能評価レポート](reports/performance-evaluation.md) / [Perfetto トレース解析・最適化レポート](reports/perfetto-trace-analysis.md) / [Ext4Windows との技術比較分析](reports/ext4windows-comparison.md) |
| `assets/` | README や設計書で参照する図表・スクリーンショット配置用ディレクトリ |
