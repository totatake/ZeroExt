# 開発・ビルド・保守ガイド

本書は **ZeroExt4** の開発者・メンテナー向けに、ビルド手順、同梱 `e2fsprogs` のアップストリーム更新手順、およびベンチマーク／Perfetto トレース取得方法をまとめたガイドです。

- エンドユーザー向けの WinFsp 導入・GUI/CLI 操作手順は [利用ガイド](usage.md) を参照してください。
- 自動テスト体系（CTest / GUI テスト / ASan / CRT リーク / `/analyze`）は [テスト・検証ガイド](../testing.md) を参照してください。
- PR 規約やライセンス境界については [CONTRIBUTING.md](../../CONTRIBUTING.md) を参照してください。

---

## 1. ビルド手順

### 1.1 必須要件（ビルド時）

- **Visual Studio 2022**（BuildTools / Community / Professional / Enterprise：`cl.exe`, `cmake.exe`, `ninja.exe`, `ctest.exe`）
- **Python 3.x**（`%LOCALAPPDATA%\Python\bin\python.exe` を優先検出し、`ext2_err.et.in` と `ext2_types.h.in` からヘッダを自動生成）
- **.NET 10 SDK**（任意：GUI アプリ `ZeroExt4Gui.exe` および `ZeroExt4.Gui.Tests` をビルド・テストする場合）
- **WinFsp SDK はビルド時不要**（[winfsp.h](../../third_party/winfsp/include/winfsp/winfsp.h) に互換 ABI 定義と `winfsp-x64.dll` 動的ローダーを内蔵しているため）。実ドライブレターへマウントする実行時のみ、WinFsp ランタイムのインストールが必要です。

### 1.2 ワンショットビルドと成果物

```powershell
# ネイティブ C11 ビルド + CTest + GUI ビルド + GUI テスト + --self-test を一括実行
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1 -Clean
```

- `-SkipGui` を指定すると C/CMake/CTest のみを実行します。
- GUI のみ単体ビルド・テストする場合は `.\scripts\build_gui.ps1` を使用します。

主な生成物（`build/` 配下）：
- `build/ext2fs.lib`: e2fsprogs (`lib/et` + `lib/ext2fs`) 静的ライブラリ
- `build/zeroext4_core.lib`: `win32_io` / `disk` / `vfs` / `extent` / `overlay` / `trace` / `winfsp_driver` コア静的ライブラリ
- `build/zeroext4.exe`: CLI ＆ WinFsp マウント実行ファイル
- `build/zeroext4_tests.exe`: 単体・結合テスト実行ファイル
- `build/zeroext4_bench.exe`: Direct I/O 性能評価ベンチマーク実行ファイル（`tests/bench_direct_io.c`）
- `build/gui/ZeroExt4Gui.exe`: デスクトップ GUI マウントマネージャー（`.NET 10` / WPF）

---

## 2. `e2fsprogs` アップストリーム更新手順

本プロジェクトでは `compat/` および `scripts/generate_e2fsprogs_headers.py` へ互換レイヤーを分離した上で（[ADR-0005](../adr/0005-zero-patch-e2fsprogs-compat-layer.md) / [詳細設計書 §2](../design/detailed-design.md#2-e2fsprogs-互換レイヤー詳細-compat--scripts)）、実上流の `e2fsprogs`（現在同梱版: `1.47.4`）を組み込んでいます（MSVC C11 コンパイル互換のための最小限調整のみ [scripts/update_e2fsprogs.ps1](../../scripts/update_e2fsprogs.ps1) で自動適用）。

新しい `e2fsprogs` バージョンを展開したディレクトリ `<path>` を指定して以下を実行すると、ソース同期・ヘッダ自動生成・ビルド・`ctest` 検証までを一括実行できます：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\update_e2fsprogs.ps1 -UpstreamDir <path\to\e2fsprogs-x.y.z>
```

---

## 3. 性能ベンチマークと Perfetto 性能トレース

### 3.1 ベンチマークツールの使い分け

- **VFS 単体・ライブマウント測定 (`zeroext4_bench.exe`)**: Direct I/O の有無による VFS インプロセス計測および WinFsp 実マウント経由のスループット計測を行います。
- **同一 NVMe SSD 5 方式横並び比較 (`scripts/bench_compare_ssd.ps1`)**: 管理者権限 PowerShell 上で Windows Native (NTFS)・`ZeroExt4`（標準 / Direct I/O）・`wsl --mount --vhd`（9P / Linux カーネル参考値）を同一物理 SSD 上で比較します。

詳細な再現コマンド・テストデータセット構成・測定結果は [性能評価レポート §4](../reports/performance-evaluation.md#4-ベンチマーク再現手順) を参照してください。

### 3.2 Perfetto トレースの取得と可視化

`zeroext4.exe mount` または `zeroext4_bench.exe` に `--trace <output.json>` を指定すると、スレッド別のコールバック・ロック競合・I/O スパンおよびキャッシュカウンタを Chrome Trace Event JSON 形式で記録します（未指定時は単一フラグ分岐のみでオーバーヘッドゼロ）。出力された `.json` ファイルはブラウザで [Perfetto UI](https://ui.perfetto.dev/) にドラッグ＆ドロップしてタイムライン解析できます。

```powershell
# マウント中の動作をトレース（Ctrl+C またはアンマウント時に書き出し）
.\build\zeroext4.exe mount --source test.img --mountpoint L: --trace .\zeroext4_mount_trace.json

# ベンチマーク実行中のロック競合・Direct I/O・LRU キャッシュ挙動をトレース
.\build\zeroext4_bench.exe --trace .\zeroext4_bench_trace.json --trace-events 524288
```

定量ボトルネック解析および最適化手法の詳細は [Perfetto トレース解析・最適化レポート](../reports/perfetto-trace-analysis.md) を参照してください。
