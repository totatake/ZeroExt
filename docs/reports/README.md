# 評価・解析レポート

本ディレクトリは **ZeroExt** の性能ベンチマーク実測結果および Perfetto トレース解析レポートを格納します。

## レポート一覧

| レポート | 内容・計測前提 |
| :--- | :--- |
| [性能評価レポート](performance-evaluation.md) | Direct I/O 有無比較（WinFsp 実マウント / VFS インプロセス）および同一 NVMe SSD 5 方式横並び比較（Native NTFS / ZeroExt / WSL2 9P / Linux Kernel） |
| [Perfetto トレース解析・最適化レポート](perfetto-trace-analysis.md) | `--trace` 出力の定量ボトルネック解析および Step 1〜4 最適化の Before / After 実測検証 |
