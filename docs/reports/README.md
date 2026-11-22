# 評価・解析レポート (`ZeroExt`)

本ディレクトリは **ZeroExt** の性能ベンチマーク実測結果および Perfetto トレース解析レポートを格納します。

## レポート一覧

| レポート | 内容・計測前提 |
| :--- | :--- |
| [performance-evaluation.md](performance-evaluation.md) | Direct I/O 有無比較（WinFsp 実マウント / VFS インプロセス）および同一 NVMe SSD 5 方式横並び比較（Native NTFS / ZeroExt / WSL2 9P / Linux Kernel） |
| [perfetto-trace-analysis.md](perfetto-trace-analysis.md) | `--trace` 出力の定量ボトルネック解析および Step 1〜4 最適化の Before / After トラッキング |
