# 性能評価レポート

本書は、Read-Only / `--overlay` モードで動作するダイレクト・エクステント I/O 経路（ロックレス extent ベース並列 `pread`、現在は既定で有効・`--no-direct-io` で標準経路へ切替可）の有無による読み取り性能の違いに加え、**同一物理 NVMe SSD 上での Windows Native (NTFS)・`ZeroExt4` (`zeroext4.exe`)・`wsl --mount --vhd` 横並び比較** の実測結果をまとめたものである。

- 対象プロジェクト: **`ZeroExt4`**（コマンド名: `zeroext4.exe`、ベンチマークバイナリ: `zeroext4_bench.exe`、評価バージョン: `v0.10.1` / Step 1〜4 最適化・`e2fsprogs 1.47.4`・オプトイン `--rw-fast-io` 分離後）
- ベンチマーク実装: [tests/bench_direct_io.c](../../tests/bench_direct_io.c)（ビルド成果物: `build/zeroext4_bench.exe`）
- 同一 SSD 比較スクリプト: [scripts/bench_compare_ssd.ps1](../../scripts/bench_compare_ssd.ps1)（管理者権限 PowerShell 実行）
- 最適化の経緯と詳細トレース解析: [Perfetto トレース解析・最適化レポート](perfetto-trace-analysis.md)
- 注記: 本書内の「`--no-direct-io` / `Std`」は標準 `ext2fs_file_read` 経路、「`--direct-io` ON / `DIO`」は RO / `--overlay` 時の既定動作に対応する。また Step 2 (`8bd61df`) 以降、RO マウント時は `FileInfoTimeout = INFINITE` (`(UINT32)-1`) が既定となり Windows NT Cache Manager が有効化されている。

---

## 1. 評価環境とワークロード構成

### 1.1 ビルド・ソフトウェア構成

| 項目 | 設定値 |
| :--- | :--- |
| ビルド構成 | CMake (`Ninja`) + MSVC 19.44 / `Release` ビルド (`scripts/build_and_test.ps1`) |
| ユーザー空間ブロックキャッシュ | `--cache-blocks 2048`（4 KB ブロック × 2048 = 8 MB LRU キャッシュ、デフォルト設定） |
| バッキングストレージ | Windows ローカルファイル上の ext4 イメージファイル（`.img`） |
| WinFsp マウントパス | `W:`（`zeroext4.exe mount --source <img_path> --mountpoint W: [--no-direct-io]`） |

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

## 2. 測定結果（`v0.10.0` / `v0.10.1`・`2026-11-27` 評価）

### 2.1 WinFsp ライブマウント経由（`.img` 上の `model.gguf`）

#### 主測定（256 MB ファイル）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `0.093 s` / `2739.8 MB/s` (`2740 IOPS`) | `0.093 s` / **`2758.6 MB/s`** (`2759 IOPS`) | **1.01x** |
| `UNBUF SEQ (64KB)` | `0.146 s` / `1752.9 MB/s` (`28046 IOPS`) | `0.140 s` / **`1832.6 MB/s`** (`29321 IOPS`) | **1.05x** |
| `UNBUF RND (4KB)` (64 MB / 16K ops) | `0.438 s` / `146.1 MB/s` (`37408 IOPS`) | `0.394 s` / **`162.4 MB/s`** (`41562 IOPS`) | **1.11x** |
| `CACHED SEQ (1024KB)` (Cold) | `0.081 s` / `3156.4 MB/s` (`3156 IOPS`) | `0.080 s` / **`3202.1 MB/s`** (`3202 IOPS`) | **1.01x** |

#### 参考（64 MB ファイル条件での実マウント比較・Step 1〜4 時点 `f173d76`）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `733.1 MB/s` | **`2862.5 MB/s`** | **3.90x** |
| `UNBUF SEQ (64KB)` | `638.9 MB/s` | **`1981.7 MB/s`** | **3.10x** |
| `UNBUF RND (4KB)` | `150.7 MB/s` | **`154.4 MB/s`** | **1.02x** |
| `CACHED SEQ (1024KB)` (Cold) | `1189.1 MB/s` | **`2474.3 MB/s`** | **2.08x** |

`v0.10.0` の標準経路バッチ連続エクステント読込により、WinFsp ライブマウント経由の `--no-direct-io` も前回 (`78cd744`) 比で `UNBUF SEQ 1024KB` が `675.8` → **`2,739.8 MB/s`（4.05x）**、`CACHED SEQ 1024KB` が `619.4` → **`3,156.4 MB/s`（5.10x）** を記録した（`v0.10.1` では安定性重視のためバッチ連続エクステント経路を `--rw --rw-fast-io` オプトインへ分離し、`--no-direct-io` は上流 `ext2fs_file_read` 準拠に戻している）。既定の Direct I/O ON はハンドル参照の `ReadPointerAcquire` 化等により `UNBUF RND (4KB)` が `142.9` → **`162.4 MB/s`（+13.6%）**・`CACHED SEQ (1024KB)` が **`3,202.1 MB/s`** と全項目で最速を維持している。

---

### 2.2 VFS 単体層（インプロセス測定、`zeroext4_bench.exe`、`v0.10.1`、`enable_direct_io=0` vs `1`）

対象 extent 構成: `/model.gguf` `runs=1`, `/frag.bin` `runs=65`

| ワークロード | `enable_direct_io=0` (上流 `ext2fs_file_read`) | `enable_direct_io=1` (既定 DIO) | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `1T Seq  64KB /model.gguf (512MB)` | `0.563 s` / `909.3 MB/s` (`14548 IOPS`, `68.74 us/op`) | `0.091 s` / **`5605.9 MB/s`** (`89695 IOPS`, `11.15 us/op`) | **6.17x** |
| `1T Seq   1MB /model.gguf (1024MB)` | `1.126 s` / `909.4 MB/s` (`909 IOPS`, `1099.63 us/op`) | `0.133 s` / **`7672.8 MB/s`** (`7673 IOPS`, `130.33 us/op`) | **8.44x** |
| `1T Seq   4KB /model.gguf (128MB)` | `0.149 s` / `859.3 MB/s` (`219990 IOPS`, `4.55 us/op`) | `0.119 s` / **`1075.7 MB/s`** (`275390 IOPS`, `3.63 us/op`) | **1.25x** |
| `1T Rnd   4KB /model.gguf (32K ops)` | `0.190 s` / `673.5 MB/s` (`172414 IOPS`, `5.80 us/op`) | `0.160 s` / **`799.3 MB/s`** (`204631 IOPS`, `4.89 us/op`) | **1.19x** |
| `8T Seq  64KB /model.gguf (2048MB)` | `2.347 s` / `872.7 MB/s` (`13964 IOPS`, `71.61 us/op`) | `0.063 s` / **`32724.2 MB/s`** (`523588 IOPS`, `1.91 us/op`) | **37.50x** |
| `8T Rnd   4KB /model.gguf (131K ops)` | `1.077 s` / `475.2 MB/s` (`121659 IOPS`, `8.22 us/op`) | `0.409 s` / **`1251.2 MB/s`** (`320310 IOPS`, `3.12 us/op`) | **2.63x** |
| `8T Rnd   4KB /frag.bin   (131K ops)` | `1.374 s` / `372.7 MB/s` (`95417 IOPS`, `10.48 us/op`) | `0.430 s` / **`1190.9 MB/s`** (`304875 IOPS`, `3.28 us/op`) | **3.20x** |

<sub>※ `v0.10.1` では `--no-direct-io` および既定 `--rw` を安全性のため上流 `e2fsprogs` (`ext2fs_file_read`/`write`) 準拠に戻したため、既定 RO（Direct I/O ON）は上流標準経路比で `1T Seq 1MB` **8.44x**、`8T Seq 64KB` **37.50x**、`8T Rnd 4KB /frag.bin` **3.20x** の優位となる（`v0.10.0` で導入したバッチ連続エクステント経路は `--rw --rw-fast-io` で利用可能）。</sub>

#### `--rw` モード比較：既定 `e2fsprogs` vs `--rw --rw-fast-io`（`zeroext4_bench.exe --rw-bench`）

256 MB テスト用 ext4 イメージ上で、既定の `--rw`（上流 `ext2fs_file_read`/`write` 準拠）と、オプトインの `--rw --rw-fast-io`（バッチ連続エクステント書込・`fs_lock` 外並列読込）を横並び計測した結果は以下の通り（`v0.10.1` 実測）。

| ワークロード | `--rw` (既定 `e2fsprogs`) | `--rw --rw-fast-io` (オプトイン) | 速度比 (`fast-io` / 既定) |
| :--- | ---: | ---: | ---: |
| `RW 1T Write Seq  1MB (64MB)`（新規割当） | `0.265 s` / `241.4 MB/s` (`241 IOPS`) | `0.016 s` / **`4091.9 MB/s`** (`4092 IOPS`, `244.39 us/op`) | **16.95x** |
| `RW 1T Write Seq 64KB (64MB)`（新規割当） | `0.279 s` / `229.0 MB/s` (`3664 IOPS`) | `0.034 s` / **`1896.9 MB/s`** (`30351 IOPS`, `32.95 us/op`) | **8.28x** |
| `RW 1T Overwrite 64KB (64MB)`（上書更新） | `0.080 s` / `805.0 MB/s` (`12881 IOPS`) | `0.023 s` / **`2814.3 MB/s`** (`45029 IOPS`, `22.21 us/op`) | **3.50x** |
| `RW 1T Write Seq  4KB (16MB)`（EOF 追記） | `0.096 s` / `166.0 MB/s` (`42496 IOPS`) | `0.052 s` / **`306.2 MB/s`** (`78393 IOPS`, `12.76 us/op`) | **1.84x** |
| `RW 1T Read  Seq  1MB (256MB)` | `0.288 s` / `889.6 MB/s` (`890 IOPS`) | `0.031 s` / **`8180.7 MB/s`** (`8181 IOPS`, `122.24 us/op`) | **9.20x** |
| `RW 8T Read  Seq 64KB (1024MB)` | `1.154 s` / `887.2 MB/s` (`14195 IOPS`) | `0.031 s` / **`32879.9 MB/s`** (`526079 IOPS`, `1.90 us/op`) | **37.06x** |
| `RW 8T Read  Rnd  4KB (65K ops)` | `0.450 s` / `569.4 MB/s` (`145763 IOPS`) | `0.122 s` / **`2095.9 MB/s`** (`536561 IOPS`, `1.86 us/op`) | **3.68x** |

---

### 2.3 同一 NVMe SSD 5 方式比較（Windows Native NTFS / `ZeroExt4` / `wsl --mount --vhd`）

#### 測定環境および条件（同一 SSD 上での公平比較）

`scripts/bench_compare_ssd.ps1` を管理者 PowerShell で実行し、全バックエンドを同一物理 NVMe SSD（`C:` ドライブ）上で計測した。`ZeroExt4` 側は `.img` ファイルの OS キャッシュ干渉を排除するため `Mount-VHD -ReadOnly` でアタッチした `\\.\PhysicalDrive4`（`win32_io.c` が実 SSD と同じ `FILE_FLAG_NO_BUFFERING` でオープン）を使用し、`wsl --mount --vhd` と同一の 1024 MB 固定サイズ VHDX（GPT Partition 1, ext4, `/model.gguf` 512 MB・`Runs: 4`）を共有した。

| 項目 | 実環境・設定値 |
| :--- | :--- |
| 計測日時 | `2026-11-27 00:00:31 +09:00` |
| CPU / RAM | `AMD Ryzen 5 5600G with Radeon Graphics` / `63.8 GB` |
| OS / WSL2 | `Windows 10 Pro 26H2 (Build 26300)` / `WSL 2.3.26.0` (`Ubuntu`, `Kernel 5.15.167.4-microsoft-standard-WSL2`) |
| 対象 SSD | `Disk 3 [INTEL SSDPEKNU512GZ]` (`Bus=NVMe`, `Media=SSD`, `476.9 GB`, `C:`) |
| テストデータ | 決定論的非ゼロ 512 MB 連続ファイル（NTFS: `native_model.gguf`, ext4: `/model.gguf` `Runs=4`） |
| 比較 5 バックエンド | ① `Native NTFS` / ② `ZeroExt4 (Standard / --no-direct-io)` / ③ `ZeroExt4 (Direct I/O 既定)` / ④ `WSL2 9P (\\wsl.localhost)` / ⑤ `WSL2 Linux Kernel (Ref)`（WSL2 内直接アクセス） |
| Cold / Unbuf 条件 | Windows 側: `FILE_FLAG_NO_BUFFERING` / WSL2 側: 各測定前に `sync && echo 3 > /proc/sys/vm/drop_caches`＋Linux 直接測定は `O_DIRECT` |
| Warm / Cached 条件 | `warm` 指定により計測前に 1 MB 単位で全 512 MB を 1 周先読みしシークバック後に計測（4KB ランダムは 16,384 ops = 64 MB） |

#### 横並びサマリー（MB/s）

| Workload | Native NTFS | `ZeroExt4 Std` (`--no-direct-io`) | `ZeroExt4 DIO` (既定) | WSL2 9P | WSL2 Kernel (Ref) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `Cold/Unbuf Seq 1024KB` | `1,930.3` | **`1,433.5`** | **`1,506.2`** | `165.4` | `1,498.1` |
| `Cold/Unbuf Seq 64KB` | `1,094.1` | **`602.5`** | **`667.5`** | `108.7` | `458.2` |
| `Cold/Unbuf Rnd 4KB` | `56.9` | `35.4` | **`36.8`** | `9.1` | `21.1` |
| `Warm/Cached Seq 1024KB` | `10,265.3` | **`9,938.8`** | **`10,664.4`** | `197.0` | `9,751.8` |
| `Warm/Cached Seq 64KB` | `8,161.6` | **`8,385.5`** | **`8,676.8`** | `124.1` | `13,403.4` |
| `Warm/Cached Rnd 4KB` | `969.6` | **`1,014.7`** | **`1,026.7`** | `18.1` | `3,154.6` |

#### 詳細実測値（所要時間 / スループット / IOPS）

<details>
<summary>5 バックエンド全 30 項目の詳細計測ログ（クリックで展開）</summary>

1. **Windows Native (NTFS)**:
   - `UNBUF SEQ (1024KB)`: `0.265 s` / `1930.3 MB/s` (`1930 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.468 s` / `1094.1 MB/s` (`17505 IOPS`)
   - `UNBUF RND (4KB)`: `1.126 s` / `56.9 MB/s` (`14557 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.050 s` / `10265.3 MB/s` (`10265 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.063 s` / `8161.6 MB/s` (`130585 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.066 s` / `969.6 MB/s` (`248212 IOPS`)
2. **`ZeroExt4` (`--no-direct-io` 標準経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.357 s` / `1433.5 MB/s` (`1433 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.850 s` / `602.5 MB/s` (`9639 IOPS`)
   - `UNBUF RND (4KB)`: `1.810 s` / `35.4 MB/s` (`9050 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.052 s` / `9938.8 MB/s` (`9939 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.061 s` / `8385.5 MB/s` (`134168 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.063 s` / `1014.7 MB/s` (`259753 IOPS`)
3. **`ZeroExt4` (Direct I/O ON 既定経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.340 s` / `1506.2 MB/s` (`1506 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.767 s` / `667.5 MB/s` (`10680 IOPS`)
   - `UNBUF RND (4KB)`: `1.739 s` / `36.8 MB/s` (`9421 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.048 s` / `10664.4 MB/s` (`10664 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.059 s` / `8676.8 MB/s` (`138829 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.062 s` / `1026.7 MB/s` (`262828 IOPS`)
4. **`WSL2 9P` (Windows アプリ → `\\wsl.localhost\Ubuntu\mnt\wsl\ext4bench\model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `3.095 s` / `165.4 MB/s` (`165 IOPS`)
   - `UNBUF SEQ (64KB)`: `4.709 s` / `108.7 MB/s` (`1740 IOPS`)
   - `UNBUF RND (4KB)`: `7.036 s` / `9.1 MB/s` (`2329 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `2.599 s` / `197.0 MB/s` (`197 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `4.127 s` / `124.1 MB/s` (`1985 IOPS`)
   - `CACHED-WARM RND (4KB)`: `3.539 s` / `18.1 MB/s` (`4630 IOPS`)
5. **`WSL2 Linux Kernel Direct` (参考値: `/mnt/wsl/ext4bench/model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `0.342 s` / `1498.1 MB/s` (`1498 IOPS`)
   - `UNBUF SEQ (64KB)`: `1.117 s` / `458.2 MB/s` (`7331 IOPS`)
   - `UNBUF RND (4KB)`: `3.031 s` / `21.1 MB/s` (`5406 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.053 s` / `9751.8 MB/s` (`9752 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.038 s` / `13403.4 MB/s` (`214454 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.020 s` / `3154.6 MB/s` (`807576 IOPS`)

</details>

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）および `--rw` 経路の高速化要因

- **Direct I/O ON（既定）**: ファイルハンドル（`ReadPointerAcquire` で無競合参照）またはシャード化 `extent_cache` に保持した `ext4_extent_map_t` に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、`fs_lock` を一切取得せず `FILE_FLAG_OVERLAPPED` 並列 `ReadFile`（`win32_io_pread_direct`）で一括読み切りを行うため、VFS 8T Seq 64KB で **`32,724.2 MB/s` (`523,588 IOPS`, `1.91 us/op`)**・1T Seq 1MB で **`7,672.8 MB/s`** に達する。
- **`--rw --rw-fast-io`（`v0.10.0` で標準経路へ導入 → `v0.10.1` でオプトイン分離）の最適化**:
  - 上流 `ext2fs_file_read` / `ext2fs_file_write` 経路は 4 KB ブロック単位で `ext2fs_bmap2` と I/O（さらに `fs_lock` 直列化）を反復するため、VFS 書込 Seq 1MB は `241.4 MB/s`、8T 読込 Seq 64KB は `872.7〜887.2 MB/s` にとどまる。
  - `--rw --rw-fast-io` では、読込側で `ctx->fs_lock` 下ではエクステント配置解決のみを行って即解放し `ctx->rwlock (Shared)` 保持のまま並列一括 `io_channel_read_blk64` を実行するよう刷新した結果、VFS 8T 読込 Seq 64KB が `887.2` → **`32,879.9 MB/s`（37.06x）**、1T 読込 Seq 1MB が `889.6` → **`8,180.7 MB/s`（9.20x）** へ劇的に改善した（`v0.10.0` 評価時の実 SSD Cold Seq 1024KB も `75.2` → **`1,433.5 MB/s`（19.06x）**）。
  - 書込側（`--rw --rw-fast-io`）でも `ext2fs_new_range` + `ext2fs_block_alloc_stats_range` による連続エクステントバッチ確保と一括 `io_channel_write_blk64`・整列ゼロコピー書込を導入し、既定 `--rw` (`241.4 MB/s`) 比で `RW 1T Write Seq 1MB` **`4,091.9 MB/s`（16.95x）**・`RW 1T Overwrite 64KB` **`2,814.3 MB/s`（3.50x）** を達成した（`v0.10.1` では想定外の独自実装バグによる破損リスクを避けるため、既定 `--rw` は `e2fsprogs` 準拠のままとし、`--rw-fast-io` 指定時のみ有効化する安全設計とした）。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **`FILE_FLAG_OVERLAPPED` と並列 I/O による多スレッド 4 KB 性能**:
   - Direct I/O 側の `8T Rnd 4KB` は `/model.gguf` **`1,251.2 MB/s`**・断片化 `/frag.bin` **`1,190.9 MB/s` (`304,875 IOPS`)** を安定して維持している。
   - 上流 `ext2fs_file_read` 準拠に戻した `enable_direct_io=0` では `/model.gguf` `475.2 MB/s`・`/frag.bin` `372.7 MB/s` となるため、RO / `--overlay` での既定 Direct I/O ON がそれぞれ **2.63x〜3.20x**（`--rw-fast-io` 時は `8T Rnd 4KB` `2,095.9 MB/s`）と大きな優位性を持つ。
2. **WinFsp 実マウント時の 4 KB 非バッファランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` はカーネル〜ユーザー空間ディスパッチ費用（約 `25〜28 us/op`）が支配的となるため、`v0.10.0` 評価時の `--direct-io` ON/OFF の差は僅差（実 SSD `36.8` vs `35.4 MB/s`）にとどまる。ただし通常のキャッシュ有効（`CACHED-WARM`）利用では NT Cache Manager により **`1,026.7 MB/s` (`262,828 IOPS`)** に達する。

### 3.3 同一 SSD 5 方式比較から得られた技術的知見・Step 1〜4 および `v0.10.0`/`v0.10.1` の効果

1. **実ブロックデバイス直読（Cold / Unbuffered）で `ZeroExt4` は `WSL2 9P` を大きく上回る**:
   - `Cold/Unbuf Seq 1024KB`: `ZeroExt4 DIO` **`1,506.2 MB/s`**（`WSL2 9P` `165.4 MB/s` の **9.11x**、`WSL2 Linux Kernel` `1,498.1 MB/s` の **1.01x**、`Native NTFS` `1,930.3 MB/s` の **78.0%**）。なお表中の `ZeroExt4 (Standard)` `1,433.5 MB/s` は `v0.10.0` で標準経路側にバッチ並列読込が有効だった時点の実測値である（`v0.10.1` 以降は `--rw --rw-fast-io` 側へ分離）。
   - `Cold/Unbuf Seq 64KB`: `ZeroExt4 DIO` **`667.5 MB/s`**（`v0.10.0 Std` `602.5 MB/s`、`WSL2 9P` `108.7 MB/s` の **6.14x**、`WSL2 Linux Kernel` `458.2 MB/s` の **1.46x**）。
   - `Cold/Unbuf Rnd 4KB`: `ZeroExt4 DIO` **`36.8 MB/s` (`9,421 IOPS`)**（`WSL2 9P` `9.1 MB/s` の **4.04x**、`WSL2 Linux Kernel` `21.1 MB/s` の **1.74x**）。
2. **Warm / Cached で `ZeroExt4` が `Native NTFS` 同等以上・`WSL2 9P` 比 54x〜70x に到達（Step 2 の効果）**:
   - Read-Only マウント時の `FileInfoTimeout` 既定値を `(UINT32)-1` (`INFINITE`) としたことで、Windows NT Cache Manager（`CcCopyRead`）が有効化され、全 Warm 項目で Native NTFS を上回る帯域を記録した：
     - `Warm/Cached Seq 1024KB`: **`10,664.4 MB/s`**（`Native NTFS` `10,265.3 MB/s` の **1.04x**、`WSL2 9P` `197.0 MB/s` の **54.1x**）
     - `Warm/Cached Seq 64KB`: **`8,676.8 MB/s`**（`Native NTFS` `8,161.6 MB/s` の **1.06x**、`WSL2 9P` `124.1 MB/s` の **69.9x**）
     - `Warm/Cached Rnd 4KB`: **`1,026.7 MB/s` (`262,828 IOPS`)**（`Native NTFS` `969.6 MB/s` の **1.06x**、`WSL2 9P` `18.1 MB/s` の **56.7x**）
3. **`WSL2 9P`（`\\wsl.localhost`）経由との比較**:
   - WSL2 内の Linux カーネル直接読み取りは Warm 時に `9,751.8〜13,403.4 MB/s`（4 KB ランダム `3,154.6 MB/s`）に達するものの、Windows 側アプリから `\\wsl.localhost` 越しにアクセスする場合は 9P / HVSocket 往復が律速となり、`197.0 MB/s`（4 KB ランダムは `18.1 MB/s`）が上限となる。`ZeroExt4` は Windows ネイティブのドライブレター経由で **Cold 時最大 9.11x・Warm 時 54x〜70x** 高速にアクセスできる。

### 3.4 実運用における使い分け指針

- **既定設定（RO / `--overlay` で Direct I/O 自動有効 + `FileInfoTimeout = INFINITE`）**:
  - 初回（Cold）の大容量・実ディスク直読（`1,506.2 MB/s`）から 2 回目以降のメモリ帯域（`10.66 GB/s` / `263K IOPS`）、さらに Step 4 の Dentry キャッシュ・`FspGetDirInfoByName` による高速な Explorer / シェルメタデータ応答までそのまま最高性能を発揮する。
- **`--rw` / `--rw-fast-io` / `--no-direct-io` / `--file-info-timeout <ms>` を検討すべきケース**:
  - 直接書き込み（`--rw`）の既定は `e2fsprogs` (`ext2fs_file_read`/`write`) 準拠として安全性を最優先する。テスト環境等で高速な直接書込／読込が必要な場合のみ `--rw --rw-fast-io` を併用することで、VFS RW 書込最大 **`4,091.9 MB/s`**・8T 読込最大 **`32,879.9 MB/s`** を引き出せる（`--rw` では整合性のため `FileInfoTimeout = 1000 ms`・Direct Extent I/O 無効を維持）。
  - 外部プロセスからイメージを同時更新するような特殊用途でカーネルキャッシュを抑制したい場合は `--file-info-timeout 1000` を指定できる。

> [!NOTE]
> Step 1〜4 の各段階における Perfetto トレース定量的効果およびメタデータ最適化（`FspGetSecurityByName` **11.97x**、`FspOpen` **27.62x**、`FspReadDirectory` **14.51x**）の詳細は [Perfetto トレース解析・最適化レポート](perfetto-trace-analysis.md) を参照。

---

## 4. ベンチマーク再現手順

### 4.1 ビルド

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_and_test.ps1
```

これにより `build\zeroext4.exe` および `build\zeroext4_bench.exe`（ソース: `tests/bench_direct_io.c`）が `Release` 構成でビルドされる。

### 4.2 VFS 単体ベンチマークの実行

引数なしで `zeroext4_bench.exe` を実行すると、`%TEMP%` に 160 MB のテスト用 ext4 イメージを自動作成し、`--no-direct-io` 相当（`enable_direct_io=0`）と RO 既定 Direct I/O ON（`enable_direct_io=1`）を連続計測したのちイメージを自動削除する。

```powershell
.\build\zeroext4_bench.exe
.\build\zeroext4_bench.exe --rw-bench
```

### 4.3 WinFsp 実マウントベンチマークの実行

1. 任意の作業用イメージを作成し、大容量ファイルを格納する（例: 320 MB イメージ + 256 MB テストファイル）。

```powershell
# 例: PowerShell で 256MB のテストファイルを作成し ext4 イメージへ格納
$HostBin = "$env:TEMP\bench_payload_256mb.bin"
$Img     = "$env:TEMP\bench_live_320mb.img"
$fs = [System.IO.File]::Create($HostBin); $fs.SetLength(256MB); $fs.Close()

.\build\zeroext4.exe mkfs $Img --size-mb 320 --label BENCH
.\build\zeroext4.exe write $Img /model.gguf $HostBin --from-file
```

2. 別ターミナルで対象ドライブをマウントし、`zeroext4_bench.exe --read` を実行する（既定 Direct I/O あり / `--no-direct-io` を切り替えて計測）。

```powershell
# マウント側（標準経路 / 既定 Direct I/O を切り替え）
.\build\zeroext4.exe mount --source $Img --mountpoint W: --no-direct-io
# または（既定 Direct I/O ON）
.\build\zeroext4.exe mount --source $Img --mountpoint W:
```

```powershell
# 計測側
.\build\zeroext4_bench.exe --read W:\model.gguf 1024 unbuf seq
.\build\zeroext4_bench.exe --read W:\model.gguf 64   unbuf seq
.\build\zeroext4_bench.exe --read W:\model.gguf 4    unbuf rnd
.\build\zeroext4_bench.exe --read W:\model.gguf 1024 cached seq
```

3. 計測完了後、マウントプロセスを停止（`Ctrl+C`）し、一時ファイル `$HostBin` および `$Img` を削除する。

### 4.4 同一 NVMe SSD 5 方式比較ベンチマーク（`scripts\bench_compare_ssd.ps1`）の実行

Windows Native (NTFS)・`ZeroExt4`（`--no-direct-io` / 既定 Direct I/O）・`wsl --mount --vhd`（9P / Linux カーネル参考値）の 5 バックエンドを同一 SSD 上で一括計測する場合は、**管理者権限の PowerShell** で以下を実行する（`Mount-VHD`, `\\.\PhysicalDriveN`, `wsl --mount --vhd` に管理者権限が必要）。

```powershell
powershell -ExecutionPolicy Bypass -File "D:\ZeroExt4\scripts\bench_compare_ssd.ps1"
```

計測ログおよび横並びサマリー表は `build\bench_ssd_compare_results.txt` へ出力され、テスト終了時に一時マウント・VHDX・一時ファイルは自動クリーンアップされる（空きドライブ `W:` が使用中の場合は `-MountDrive Y:` 等で変更可能）。
