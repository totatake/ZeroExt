# 性能評価報告書 (`--direct-io` 有無の比較)

本書は、Read-Only / `--overlay` モードで有効化できる `--direct-io` オプション（ロックレス extent ベース並列 `pread` 経路）の有無による読み取り性能の違いを、実環境（WinFsp ライブマウント経由）および VFS 単体層（インプロセス）の 2 階層で評価した結果をまとめたものである。

- 対象リポジトリ状態: コミット `e41a034`（`feat(vfs,cli): require explicit --direct-io (RO/overlay only), default to RO, and validate overlay lower-ID`）
- ベンチマーク実装: [tests/bench_direct_io.c](../tests/bench_direct_io.c)（ビルド成果物: `build/ext4fsp_bench.exe`）

---

## 1. 評価環境とワークロード構成

### 1.1 ビルド・ソフトウェア構成

| 項目 | 設定値 |
| :--- | :--- |
| ビルド構成 | CMake (`Ninja`) + MSVC 19.44 / `Release` ビルド (`scripts/build_and_test.ps1`) |
| ユーザー空間ブロックキャッシュ | `--cache-blocks 2048`（4 KB ブロック × 2048 = 8 MB LRU キャッシュ、デフォルト設定） |
| バッキングストレージ | Windows ローカルファイル上の ext4 イメージファイル（`.img`） |
| WinFsp マウントパス | `W:`（`ext4fsp.exe mount --source <img_path> --mountpoint W: [--direct-io]`） |

### 1.2 テストデータセット

1. **実マウント（WinFsp 経由）評価**:
   - **主測定**: 320 MB ext4 イメージ上に配置した **256 MB** 連続ファイル `W:\model.gguf`（LLM モデル等のシーケンシャル / `mmap` 想定ワークロード）。
   - **参考測定**: 同一条件での **64 MB** ファイル測定も実施し、サイズ依存の傾向をクロスチェック。
   - **アクセス種別**: Win32 `CreateFileW` / `ReadFile` により以下を測定。
     - `UNBUF SEQ (1024KB)` / `(64KB)`: `FILE_FLAG_NO_BUFFERING` + 4 KB アラインバッファによる 256 MB 全域シーケンシャル読み取り
     - `UNBUF RND (4KB)`: `FILE_FLAG_NO_BUFFERING` による 4 KB ランダムシーク読み取り（16,384 ops = 64 MB 読み取り）
     - `CACHED SEQ (1024KB)`: `FILE_FLAG_SEQUENTIAL_SCAN`（OS ページキャッシュ併用）での 256 MB 全域シーケンシャル読み取り
2. **VFS 単体層（インプロセス）評価**:
   - 160 MB ext4 イメージに以下 2 種類のファイルを生成。
     - `/model.gguf`（64 MB, `runs=1` の連続 extent）
     - `/frag.bin`（32 MB, 512 KB 単位で他ファイル `/sep.bin` と交互書き込みした断片化ファイル, `runs=65`）
   - 測定対象:
     - 1 スレッド シーケンシャル（64 KB × 8,192 = 512 MB / 1 MB × 1,024 = 1,024 MB / 4 KB × 32,768 = 128 MB）
     - 1 スレッド ランダム 4 KB（32,768 ops = 128 MB）
     - 8 スレッド 並列シーケンシャル 64 KB（4,096 ops/thread × 8 = 2,048 MB）
     - 8 スレッド 並列ランダム 4 KB（16,384 ops/thread × 8 = 131,072 ops = 512 MB、`/model.gguf` および `/frag.bin`）

---

## 2. 測定結果

### 2.1 WinFsp ライブマウント経由（`W:\model.gguf` 256 MB）

#### 主測定（256 MB ファイル）

| ワークロード | `--direct-io` OFF | `--direct-io` ON | 速度比 (ON / OFF) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `0.291 s` / `881.0 MB/s` (`881 IOPS`) | `0.098 s` / `2619.9 MB/s` (`2620 IOPS`) | **2.97x** |
| `UNBUF SEQ (64KB)` | `0.313 s` / `818.2 MB/s` (`13092 IOPS`) | `0.129 s` / `1989.0 MB/s` (`31824 IOPS`) | **2.43x** |
| `UNBUF RND (4KB)` (64 MB / 16K ops) | `0.439 s` / `145.9 MB/s` (`37358 IOPS`) | `0.379 s` / `168.7 MB/s` (`43177 IOPS`) | **1.16x** |
| `CACHED SEQ (1024KB)` | `0.296 s` / `864.3 MB/s` (`864 IOPS`) | `0.104 s` / `2465.5 MB/s` (`2465 IOPS`) | **2.85x** |

#### 参考（64 MB ファイル条件での実マウント比較）

| ワークロード | `--direct-io` OFF | `--direct-io` ON | 速度比 (ON / OFF) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `898.6 MB/s` | `2784.9 MB/s` | **3.10x** |
| `UNBUF SEQ (64KB)` | `819.3 MB/s` | `1965.8 MB/s` | **2.40x** |
| `UNBUF RND (4KB)` | `146.7 MB/s` | `160.0 MB/s` | **1.09x** |
| `CACHED SEQ (1024KB)` | `887.8 MB/s` | `2360.6 MB/s` | **2.66x** |

64 MB / 256 MB の両条件で一貫して大ブロック・シーケンシャル読み取りにおいて約 2.4x〜3.1x の高速化が確認された。

---

### 2.2 VFS 単体層（インプロセス測定、`.\build\ext4fsp_bench.exe`、`enable_direct_io=0` vs `1`）

対象 extent 構成: `/model.gguf` `runs=1`, `/frag.bin` `runs=65`

| ワークロード | `enable_direct_io=0` (標準経路) | `enable_direct_io=1` (`--direct-io`) | 速度比 (ON / OFF) |
| :--- | ---: | ---: | ---: |
| `1T Seq  64KB /model.gguf (512MB)` | `0.429 s` / `1193.3 MB/s` (`19093 IOPS`, `52.38 us/op`) | `0.083 s` / `6135.0 MB/s` (`98161 IOPS`, `10.19 us/op`) | **5.14x** |
| `1T Seq   1MB /model.gguf (1024MB)` | `0.855 s` / `1197.2 MB/s` (`1197 IOPS`, `835.25 us/op`) | `0.165 s` / `6212.1 MB/s` (`6212 IOPS`, `160.98 us/op`) | **5.19x** |
| `1T Seq   4KB /model.gguf (128MB)` | `0.143 s` / `893.7 MB/s` (`228783 IOPS`, `4.37 us/op`) | `0.086 s` / `1489.4 MB/s` (`381282 IOPS`, `2.62 us/op`) | **1.67x** |
| `1T Rnd   4KB /model.gguf (32K ops)` | `0.186 s` / `689.9 MB/s` (`176606 IOPS`, `5.66 us/op`) | `0.129 s` / `994.3 MB/s` (`254542 IOPS`, `3.93 us/op`) | **1.44x** |
| `8T Seq  64KB /model.gguf (2048MB)` | `0.723 s` / `2834.1 MB/s` (`45345 IOPS`, `22.05 us/op`) | `0.379 s` / `5404.1 MB/s` (`86466 IOPS`, `11.57 us/op`) | **1.91x** |
| `8T Rnd   4KB /model.gguf (131K ops)` | `0.512 s` / `1000.4 MB/s` (`256093 IOPS`, `3.90 us/op`) | `0.810 s` / `632.4 MB/s` (`161906 IOPS`, `6.18 us/op`) | **0.63x** |
| `8T Rnd   4KB /frag.bin   (131K ops)` | `0.630 s` / `812.1 MB/s` (`207898 IOPS`, `4.81 us/op`) | `0.799 s` / `640.6 MB/s` (`163987 IOPS`, `6.10 us/op`) | **0.79x** |

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）が大幅に高速化される理由

- **VFS 単体で最大 5.19x（`6212.1 MB/s`）、WinFsp 実マウントでも最大 2.97x（`2619.9 MB/s`）を達成。**
- 標準経路（`--direct-io` OFF）は、任意の読込サイズであっても内部で `ext2fs_file_open` / `ext2fs_file_lseek` / `ext2fs_file_read` を介して **4 KB ブロック単位の `cache_lock` 取得・LRU 探索・コピー**を繰り返し実行するため、1 MB 読込 1 回あたり約 256 回のブロック単位オーバーヘッド（`835.25 us/op`）が発生する。
- `--direct-io` ON では、キャッシュ済み extent テーブル（`ext4_extent_map_t`）に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、1 回の `win32_io_pread_direct`（ロックレス並列 `ReadFile` + `OVERLAPPED.Offset`）で最大 1 MB まとめて読み切るため、VFS レイテンシが `160.98 us/op` まで短縮される。
- 実マウント環境では WinFsp のカーネル〜ユーザー空間 IRP 往復コストやファイルシステムのオーバーヘッドが含まれるため絶対スループットは 2.6 GB/s 前後で頭打ちになるものの、それでも非 Direct I/O 比 **2.43x〜2.97x** の実効高速化が得られる。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **ユーザー空間 8 MB LRU キャッシュヒットによる逆転（8 スレッドランダム 4 KB で 0.63x〜0.79x）**:
   - `/model.gguf`（64 MB）および `/frag.bin`（32 MB）は、デフォルトの 8 MB ユーザー空間 LRU キャッシュ（`2048` ブロック × 4 KB）に作業集合のそれぞれ約 12.5%〜25% が収まる。
   - 標準経路（`enable_direct_io=0`）はキャッシュヒット時に Windows の `ReadFile` システムコールを発行せずユーザー空間メモリコピーのみで完了できるため、特に 8 スレッド並列ランダム 4 KB では `1000.4 MB/s`（`/model.gguf`）・`812.1 MB/s`（`/frag.bin`）に達する。
   - 一方 `--direct-io` 経路（`enable_direct_io=1`）は LRU キャッシュを完全にバイパスして毎回 `ReadFile` システムコールを発行するため、小容量・並列ランダム読込ではシステムコール発行や OS ファイルシステム内の競合が支配的になり、`0.63x`（`/model.gguf`）・`0.79x`（`/frag.bin`）と標準経路を下回る場合がある。
2. **WinFsp 実マウント時の 4 KB ランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` は 1 回あたり約 23〜27 us/op を要し、カーネル〜ユーザー空間間のディスパッチ費用がボトルネックとなるため、`--direct-io` ON/OFF の差は **1.16x**（`168.7` vs `145.9 MB/s`）にとどまる。
   - また、単一スレッドの 4 KB ランダム（`1T Rnd 4KB`）では `--direct-io` 側が `ext2fs_file_open/read` 経路の関数コール・ブロック境界計算オーバーヘッドを省けるため **1.44x**（`994.3` vs `689.9 MB/s`）と優位になるが、8 スレッド時はシステムコール並列発行の影響で標準経路との逆転または拮抗が生じる。

### 3.3 実運用における使い分け指針

- **推奨（`--direct-io` を有効化すべきケース）**:
  - 大容量ファイルのシーケンシャル / ストリーミング / `mmap` 読込（LLM GGUF モデル、VM イメージ、大容量動画・アーカイブ、ビルドアーティファクト読込など）。
  - Read-Only（デフォルト）または `--overlay` モードで下位 ext4 を読み取り専用として扱う運用。
- **標準モードのまま（`--direct-io` 指定不要）が望ましいケース**:
  - 通常のシェル操作、ソースツリー参照、メタデータ操作や小ファイル（4 KB 前後）への高局所性ランダムアクセス中心のワークロード（`win32_io` の 8 MB ユーザー空間 LRU キャッシュが有効に機能する）。
  - 注意: `--rw` モードでは整合性維持のため `--direct-io` は利用不可（CLI でエラー終了する）。

---

## 4. ベンチマーク再現手順

### 4.1 ビルド

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_and_test.ps1
```

これにより `build\ext4fsp.exe` および `build\ext4fsp_bench.exe`（ソース: `tests/bench_direct_io.c`）が `Release` 構成でビルドされる。

### 4.2 VFS 単体ベンチマークの実行

引数なしで `ext4fsp_bench.exe` を実行すると、`%TEMP%` に 160 MB のテスト用 ext4 イメージを自動作成し、`enable_direct_io=0` と `enable_direct_io=1` を連続計測したのちイメージを自動削除する。

```powershell
.\build\ext4fsp_bench.exe
```

### 4.3 WinFsp 実マウントベンチマークの実行

1. 任意の作業用イメージを作成し、大容量ファイルを格納する（例: 320 MB イメージ + 256 MB テストファイル）。

```powershell
# 例: PowerShell で 256MB のテストファイルを作成し ext4 イメージへ格納
$HostBin = "$env:TEMP\bench_payload_256mb.bin"
$Img     = "$env:TEMP\bench_live_320mb.img"
$fs = [System.IO.File]::Create($HostBin); $fs.SetLength(256MB); $fs.Close()

.\build\ext4fsp.exe mkfs $Img --size 320M --label BENCH
.\build\ext4fsp.exe write $Img /model.gguf --from-file $HostBin
```

2. 別ターミナルで対象ドライブをマウントし、`ext4fsp_bench.exe --winfsp-read` を実行する（`--direct-io` なし / ありを切り替えて計測）。

```powershell
# マウント側（標準モード / Direct I/O モードを切り替え）
.\build\ext4fsp.exe mount --source $Img --mountpoint W:
# または
.\build\ext4fsp.exe mount --source $Img --mountpoint W: --direct-io
```

```powershell
# 計測側
.\build\ext4fsp_bench.exe --winfsp-read W:\model.gguf 1024 unbuf seq
.\build\ext4fsp_bench.exe --winfsp-read W:\model.gguf 64   unbuf seq
.\build\ext4fsp_bench.exe --winfsp-read W:\model.gguf 4    unbuf rnd
.\build\ext4fsp_bench.exe --winfsp-read W:\model.gguf 1024 cached seq
```

3. 計測完了後、マウントプロセスを停止（`Ctrl+C`）し、一時ファイル `$HostBin` および `$Img` を削除する。
