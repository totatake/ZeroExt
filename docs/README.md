# ZeroExt ドキュメント一覧

本ディレクトリは **ZeroExt**（`libext2fs` + `WinFsp` による Windows 向け高速ユーザー空間 ext2/ext3/ext4 ドライバ・CLI・WPF GUI）の技術ドキュメントを集約するハブです。

## 基本ドキュメント

| ドキュメント | 内容 |
| :--- | :--- |
| [overview.md](overview.md) | 開発背景、2-Tier I/O バイパス・技術的特徴、他手法比較（NTFS / WSL2 9P 等）、実践ユースケース |
| [requirements.md](requirements.md) | ソフトウェア要件仕様書（対応スコープ・プロジェクト境界・機能要件 `FR-01`..`11` / 非機能要件 `NFR-01`..`04`） |
| [architecture.md](architecture.md) | システムアーキテクチャ設計書（コンポーネント図・シーケンス図・`e2fsprogs` 追従構造・ADR サマリー） |
| [testing.md](testing.md) | テスト仕様・検証ガイド（CTest 6 スイート 32 関数・GUI テスト 11 件・ASan / CRT リーク検出 / `/analyze`） |
| [roadmap.md](roadmap.md) | 完了済みマイルストーン（`v0.1.0`〜`v0.8.0`）および Phase 1〜4 開発ロードマップ |

## サブディレクトリ

| ディレクトリ | 用途 |
| :--- | :--- |
| [guides/usage.md](guides/usage.md) | ビルド手順・`ZeroExtGui.exe` 操作・CLI リファレンス・WinFsp 導入＆マウント手順・`e2fsprogs` 更新手順 |
| [adr/README.md](adr/README.md) | Architecture Decision Records（`ADR-0001`〜`ADR-0013`: WinFsp 採用・ライセンス分離・ロック階層・Direct I/O・Overlay 等） |
| [design/README.md](design/README.md) | 詳細設計ハブ（[detailed-design.md](design/detailed-design.md) および将来のサブシステム設計パック） |
| [reports/README.md](reports/README.md) | 性能ベンチマーク報告（[performance-evaluation.md](reports/performance-evaluation.md)）・Perfetto トレース解析（[perfetto-trace-analysis.md](reports/perfetto-trace-analysis.md)） |
| `assets/` | README や設計書で参照する図表・スクリーンショット配置用ディレクトリ |
