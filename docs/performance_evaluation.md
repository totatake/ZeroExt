# 性能評価報告書 (Direct I/O 比較 / Windows NTFS・WSL2 同一 SSD 比較)

本書は、Read-Only / `--overlay` モードで動作するダイレクト・エクステント I/O 経路（ロックレス extent ベース並列 `pread`、現在は既定で有効・`--no-direct-io` で標準経路へ切替可）の有無による読み取り性能の違いに加え、**同一物理 NVMe SSD 上での Windows Native (NTFS)・`ZeroExt` (`zeroext.exe`)・`wsl --mount --vhd` 横並び比較** の実測結果をまとめたものである。

- 対象プロジェクト: **`ZeroExt`**（コマンド名: `zeroext.exe`、ベンチマークバイナリ: `zeroext_bench.exe`）
- ベンチマーク実装: [tests/bench_direct_io.c](../tests/bench_direct_io.c)（ビルド成果物: `build/zeroext_bench.exe`）
- 同一 SSD 比較スクリプト: [scripts/bench_compare_ssd.ps1](../scripts/bench_compare_ssd.ps1)（管理者権限 PowerShell 実行）
- 注記: 本書内の比較測定当時は `--direct-io` が明示フラグで既定 OFF だったため、「`--direct-io OFF` / `Std`」は現在の `--no-direct-io`（標準経路）、「`--direct-io ON` / `DIO`」は現在の RO / Overlay 既定動作に対応する。

---

## 1. 評価環境とワークロード構成

### 1.1 ビルド・ソフトウェア構成

| 項目 | 設定値 |
| :--- | :--- |
| ビルド構成 | CMake (`Ninja`) + MSVC 19.44 / `Release` ビルド (`scripts/build_and_test.ps1`) |
| ユーザー空間ブロックキャッシュ | `--cache-blocks 2048`（4 KB ブロック × 2048 = 8 MB LRU キャッシュ、デフォルト設定） |
| バッキングストレージ | Windows ローカルファイル上の ext4 イメージファイル（`.img`） |
| WinFsp マウントパス | `W:`（`zeroext.exe mount --source <img_path> --mountpoint W: [--no-direct-io]`） |

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

### 2.2 VFS 単体層（インプロセス測定、`zeroext_bench.exe`、`enable_direct_io=0` vs `1`）

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

### 2.3 同一 NVMe SSD 5 方式比較（Windows Native NTFS / `ZeroExt` / `wsl --mount --vhd`）

#### 測定環境および条件（同一 SSD 上での公平比較）

`scripts/bench_compare_ssd.ps1` を管理者 PowerShell で実行し、全バックエンドを同一物理 NVMe SSD（`C:` ドライブ）上で計測した。`ZeroExt` 側は `.img` ファイルの OS キャッシュ干渉を排除するため `Mount-VHD -ReadOnly` でアタッチした `\\.\PhysicalDrive4`（`win32_io.c` が実 SSD と同じ `FILE_FLAG_NO_BUFFERING` でオープン）を使用し、`wsl --mount --vhd` と同一の 1024 MB 固定サイズ VHDX（GPT Partition 1, ext4, `/model.gguf` 512 MB・`Runs: 4`）を共有した。

| 項目 | 実環境・設定値 |
| :--- | :--- |
| 計測日時 | `2026-11-19 21:50:53 +09:00` |
| CPU / RAM | `AMD Ryzen 5 5600G with Radeon Graphics` / `63.8 GB` |
| OS / WSL2 | `Windows 10 Pro 26H2 (Build 26300)` / `WSL 2.3.26.0` (`Ubuntu`, `Kernel 5.15.167.4-microsoft-standard-WSL2`) |
| 対象 SSD | `Disk 3 [INTEL SSDPEKNU512GZ]` (`Bus=NVMe`, `Media=SSD`, `476.9 GB`, `C:`) |
| テストデータ | 決定論的非ゼロ 512 MB 連続ファイル（NTFS: `native_model.gguf`, ext4: `/model.gguf` `Runs=4`） |
| 比較 5 バックエンド | ① `Native NTFS` / ② `ZeroExt (Standard / --no-direct-io)` / ③ `ZeroExt (Direct I/O 既定)` / ④ `WSL2 9P (\\wsl.localhost)` / ⑤ `WSL2 Linux Kernel (Ref)`（WSL2 内直接アクセス） |
| Cold / Unbuf 条件 | Windows 側: `FILE_FLAG_NO_BUFFERING` / WSL2 側: 各測定前に `sync && echo 3 > /proc/sys/vm/drop_caches`＋Linux 直接測定は `O_DIRECT` |
| Warm / Cached 条件 | `warm` 指定により計測前に 1 MB 単位で全 512 MB を 1 周先読みしシークバック後に計測（4KB ランダムは 16,384 ops = 64 MB） |

#### 横並びサマリー（MB/s）

| Workload | Native NTFS | `ZeroExt Std` (`--no-direct-io`) | `ZeroExt DIO` (既定) | WSL2 9P | WSL2 Kernel (Ref) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `Cold/Unbuf Seq 1024KB` | `1,350.0` | `52.1` | **`1,490.9`** | `161.7` | `1,758.4` |
| `Cold/Unbuf Seq 64KB` | `1,072.0` | `64.5` | **`676.4`** | `107.0` | `448.8` |
| `Cold/Unbuf Rnd 4KB` | `30.1` | `33.1` | **`37.6`** | `9.6` | `25.7` |
| `Warm/Cached Seq 1024KB` | `10,953.9` | `73.4` | `1,490.5` | `185.4` | `9,990.4` |
| `Warm/Cached Seq 64KB` | `8,763.7` | `68.1` | `661.2` | `115.6` | `13,451.3` |
| `Warm/Cached Rnd 4KB` | `986.2` | `32.9` | `36.8` | `17.4` | `3,263.8` |

#### 詳細実測値（所要時間 / スループット / IOPS）

<details>
<summary>5 バックエンド全 30 項目の詳細計測ログ（クリックで展開）</summary>

1. **Windows Native (NTFS)**:
   - `UNBUF SEQ (1024KB)`: `0.379 s` / `1350.0 MB/s` (`1350 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.478 s` / `1072.0 MB/s` (`17152 IOPS`)
   - `UNBUF RND (4KB)`: `2.125 s` / `30.1 MB/s` (`7709 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.047 s` / `10953.9 MB/s` (`10954 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.058 s` / `8763.7 MB/s` (`140219 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.065 s` / `986.2 MB/s` (`252458 IOPS`)
2. **`ZeroExt` (`--no-direct-io` 標準経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `9.835 s` / `52.1 MB/s` (`52 IOPS`)
   - `UNBUF SEQ (64KB)`: `7.938 s` / `64.5 MB/s` (`1032 IOPS`)
   - `UNBUF RND (4KB)`: `1.934 s` / `33.1 MB/s` (`8471 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `6.976 s` / `73.4 MB/s` (`73 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `7.513 s` / `68.1 MB/s` (`1090 IOPS`)
   - `CACHED-WARM RND (4KB)`: `1.943 s` / `32.9 MB/s` (`8431 IOPS`)
3. **`ZeroExt` (Direct I/O ON 既定経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.343 s` / `1490.9 MB/s` (`1491 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.757 s` / `676.4 MB/s` (`10822 IOPS`)
   - `UNBUF RND (4KB)`: `1.701 s` / `37.6 MB/s` (`9634 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.344 s` / `1490.5 MB/s` (`1491 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.774 s` / `661.2 MB/s` (`10579 IOPS`)
   - `CACHED-WARM RND (4KB)`: `1.740 s` / `36.8 MB/s` (`9418 IOPS`)
4. **`WSL2 9P` (Windows アプリ → `\\wsl.localhost\Ubuntu\mnt\wsl\ext4bench\model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `3.166 s` / `161.7 MB/s` (`162 IOPS`)
   - `UNBUF SEQ (64KB)`: `4.784 s` / `107.0 MB/s` (`1712 IOPS`)
   - `UNBUF RND (4KB)`: `6.637 s` / `9.6 MB/s` (`2469 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `2.762 s` / `185.4 MB/s` (`185 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `4.428 s` / `115.6 MB/s` (`1850 IOPS`)
   - `CACHED-WARM RND (4KB)`: `3.687 s` / `17.4 MB/s` (`4444 IOPS`)
5. **`WSL2 Linux Kernel Direct` (参考値: `/mnt/wsl/ext4bench/model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `0.291 s` / `1758.4 MB/s` (`1758 IOPS`)
   - `UNBUF SEQ (64KB)`: `1.141 s` / `448.8 MB/s` (`7181 IOPS`)
   - `UNBUF RND (4KB)`: `2.489 s` / `25.7 MB/s` (`6583 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.051 s` / `9990.4 MB/s` (`9990 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.038 s` / `13451.3 MB/s` (`215220 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.020 s` / `3263.8 MB/s` (`835526 IOPS`)

</details>

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）が大幅に高速化される理由

- **VFS 単体で最大 5.19x（`6212.1 MB/s`）、WinFsp 実マウントでも最大 2.97x（`2619.9 MB/s`）を達成。**
- 標準経路（`--direct-io` OFF）は、任意の読込サイズであっても内部で `ext2fs_file_open` / `ext2fs_file_lseek` / `ext2fs_file_read` を介して **4 KB ブロック単位の `cache_lock` 取得・LRU 探索・コピー**を繰り返し実行するため、1 MB 読込 1 回あたり約 256 回のブロック単位オーバーヘッド（`835.25 us/op`）が発生する。
- `--direct-io` ON では、キャッシュ済み extent テーブル（`ext4_extent_map_t`）に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、1 回の `win32_io_pread_direct`（ロックレス並列 `ReadFile` + `OVERLAPPED.Offset`）で最大 1 MB まとめて読み切るため、VFS レイテンシが `160.98 us/op` まで短縮される。
- 実マウント環境では WinFsp のカーネル〜ユーザー空間 IRP 往復コストやファイルシステムのオーバーヘッドが含まれるため絶対スループットは 2.6 GB/s 前後で頭打ちになるものの、それでも非 Direct I/O 比 **2.43x〜2.97x** の実効高速化が得られる。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **ユーザー空間 8 MB LRU キャッシュヒットによる逆転（8 スレッドランダム 4 KB で 0.63x〜0.79x）とカーネル側 `ReadFile` 直列化**:
   - `/model.gguf`（64 MB）および `/frag.bin`（32 MB）は、デフォルトの 8 MB ユーザー空間 LRU キャッシュ（`2048` ブロック × 4 KB）に作業集合のそれぞれ約 12.5%〜25% が収まる。
   - 標準経路（`enable_direct_io=0`）はキャッシュヒット時に Windows の `ReadFile` システムコールを発行せずユーザー空間メモリコピーのみで完了できるため、特に 8 スレッド並列ランダム 4 KB では `1000.4 MB/s`（`/model.gguf`）・`812.1 MB/s`（`/frag.bin`）に達する。
   - 一方 `--direct-io` 経路（`enable_direct_io=1`）は LRU キャッシュをバイパスして毎回 `ReadFile` を発行するためシステムコール負荷が増えることに加え、**Perfetto トレース解析により `src/io/win32_io.c` が `FILE_FLAG_OVERLAPPED` なしで同期 `HANDLE` を開いているため Windows カーネル `FileObject->Lock` が全スレッドの `ReadFile` を直列化し（`8T Rnd 4KB` の `ReadFile` 平均が `1T` の `3.65 µs` → `49.11 µs` へ 13.5 倍悪化）、Lock Convoy を引き起こしていることが真因として特定された**（詳細は [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md) を参照）。
2. **WinFsp 実マウント時の 4 KB ランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` は 1 回あたり約 23〜27 us/op を要し、カーネル〜ユーザー空間間のディスパッチ費用がボトルネックとなるため、`--direct-io` ON/OFF の差は **1.16x**（`168.7` vs `145.9 MB/s`）にとどまる。
   - また、単一スレッドの 4 KB ランダム（`1T Rnd 4KB`）では `--direct-io` 側が `ext2fs_file_open/read` 経路の関数コール・ブロック境界計算オーバーヘッドを省けるため **1.44x**（`994.3` vs `689.9 MB/s`）と優位になるが、8 スレッド時はシステムコール並列発行の影響で標準経路との逆転または拮抗が生じる。

### 3.3 同一 SSD 5 方式比較から得られた技術的知見・ボトルネック分析

1. **実ブロックデバイス直読（Cold / Unbuffered）で `ZeroExt`（Direct I/O 既定）は Windows Native NTFS 相当・WSL2 9P 比最大 9.22x を達成**:
   - `Cold/Unbuf Seq 1024KB`: **`1,490.9 MB/s`**（`--no-direct-io` 標準経路 `52.1 MB/s` の **28.6x**、`WSL2 9P` `161.7 MB/s` の **9.22x**、`Native NTFS` `1,350.0 MB/s` の **1.10x**、`WSL2 Linux Kernel` `1,758.4 MB/s` の **84.8%**）。
   - `Cold/Unbuf Seq 64KB`: **`676.4 MB/s`**（`--no-direct-io` 標準経路の **10.5x**、`WSL2 9P` の **6.32x**、`WSL2 Linux Kernel` `448.8 MB/s` の **1.51x**）。
   - `Cold/Unbuf Rnd 4KB`: **`37.6 MB/s` (`9,634 IOPS`)**（`WSL2 9P` `9.6 MB/s` の **3.92x**、`Native NTFS` `30.1 MB/s` の **1.25x**、`WSL2 Linux Kernel` `25.7 MB/s` の **1.46x**）。
2. **`--no-direct-io` 標準経路が実ディスク（`PhysicalDriveN`）で `52.1 MB/s` まで低下する理由（Direct I/O 既定有効化の根拠）**:
   - セクション 2.1 の `.img` ファイル評価ではホスト OS のページキャッシュ（[win32_io.c](../src/io/win32_io.c#L451) が `.img` に `FILE_FLAG_RANDOM_ACCESS` を指定）が効いていたため `881.0 MB/s` が出ていたが、実ディスク `\\.\PhysicalDriveN` 時は `FILE_FLAG_NO_BUFFERING` で開くため OS 先読みが効かない。
   - この状態で `ext2fs_file_read` 経由の標準経路を使うと **1 MB 読込ごとに 256 回の非バッファ Queue Depth 1 の 4 KB 読込**が NVMe へ発行され `52.1 MB/s` が上限となる。Direct I/O はエクステント結合（4 runs）により 1 回の 1 MB `ReadFile` へまとめるため、実物理 SSD 環境では特に効果が大きく **28.6x（`1,490.9 MB/s`）** へ跳ね上がる。これが `ZeroExt` で RO / `--overlay` 時の Direct I/O 既定有効化を決定した主要因である。
3. **`ZeroExt` で Warm/Cached と Cold/Unbuf が同水準になる理由と今後の最適化ポイント**:
   - `Native NTFS`（`10,953.9 MB/s`）および `WSL2 Linux Kernel`（`9,990.4 MB/s`）は Warm 時に OS ページキャッシュからメモリ帯域で応答する一方、`ZeroExt` は Warm 時も `1,490.5 MB/s`（`--no-direct-io` は `73.4 MB/s`）にとどまった。
   - 原因は [winfsp_driver.c](../src/driver/winfsp_driver.c#L607) の `FileInfoTimeout = 1000` (ms) にある。WinFsp カーネルドライバは `FileInfoTimeout = INFINITE`（`(UINT32)-1`）のときのみ Windows NT Cache Manager（`CcCopyRead`）によるファイルデータキャッシュを有効化する仕様であるため、現状の有限値（`1000`）ではキャッシュウォーム時も毎回ユーザー空間の `FspRead` 経由でディスク直読が発生している。Read-Only マウント時（またはオプション指定時）に `FileInfoTimeout = (UINT32)-1` へ切り替えることで、Warm 性能を Native NTFS 水準へ引き上げられる余地がある。
4. **`WSL2 9P`（`\\wsl.localhost`）経由の制約**:
   - WSL2 内の Linux カーネル直接読み取りは Warm 時に `9,990〜13,451 MB/s` に達するものの、Windows 側アプリケーションが `\\wsl.localhost`（9P クライアントリダイレクタ）越しにアクセスする場合は Hyper-V 仮想ネットワーク（HVSocket）/ Plan9 RPC 往復が支配的となり、キャッシュウォーム時でも `185.4 MB/s`（4 KB ランダムは `17.4 MB/s`）が上限となる。これに対し `ZeroExt`（Direct I/O 既定 ON）は Windows ネイティブのドライブレター経由で **約 8 倍〜9 倍** 高速にアクセスできる。

### 3.4 実運用における使い分け指針

- **既定設定（RO / `--overlay` で Direct I/O 自動有効）**:
  - 実ディスク直読での大容量ファイル・シーケンシャル / ストリーミング / `mmap` 読込（LLM GGUF モデル、VM イメージ、大容量動画・アーカイブ、ビルドアーティファクト読込など）から日常参照までそのまま最高性能を発揮する（28.6x vs 標準経路）。
- **`--no-direct-io` を検討すべきケース**:
  - `.img` ファイルや 8 MB LRU キャッシュ内に作業集合が高密度に収まる特殊な 4 KB 多スレッドランダム読込（例: 32 MB 断片化ファイルの 8T ランダム読込等）でユーザー空間キャッシュヒットを優先したい場合。
  - 注意: `--rw` モードでは整合性維持のため Direct I/O は自動的に無効化され、`--direct-io` 明示併用時はエラー終了する。

> [!NOTE]
> WinFsp コールバック、`ctx->rwlock` / `cache_lock` のロック競合、`ReadFile` 待機、および Overlay Copy-up の詳細なタイムライン計測については **Perfetto トレース出力機能**（`--trace`）が実装済みである。`zeroext_bench.exe --trace` およびライブマウントの実測トレース解析から得られた原因特定と改善計画は [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md)（機能構想は [今後の構想・ロードマップ](future_roadmap.md#2-性能解析用-perfetto-トレース出力機能の追加実装済み)）を参照。

---

## 4. ベンチマーク再現手順

### 4.1 ビルド

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_and_test.ps1
```

これにより `build\zeroext.exe` および `build\zeroext_bench.exe`（ソース: `tests/bench_direct_io.c`）が `Release` 構成でビルドされる。

### 4.2 VFS 単体ベンチマークの実行

引数なしで `zeroext_bench.exe` を実行すると、`%TEMP%` に 160 MB のテスト用 ext4 イメージを自動作成し、`--no-direct-io` 相当（`enable_direct_io=0`）と RO 既定 Direct I/O ON（`enable_direct_io=1`）を連続計測したのちイメージを自動削除する。

```powershell
.\build\zeroext_bench.exe
```

### 4.3 WinFsp 実マウントベンチマークの実行

1. 任意の作業用イメージを作成し、大容量ファイルを格納する（例: 320 MB イメージ + 256 MB テストファイル）。

```powershell
# 例: PowerShell で 256MB のテストファイルを作成し ext4 イメージへ格納
$HostBin = "$env:TEMP\bench_payload_256mb.bin"
$Img     = "$env:TEMP\bench_live_320mb.img"
$fs = [System.IO.File]::Create($HostBin); $fs.SetLength(256MB); $fs.Close()

.\build\zeroext.exe mkfs $Img --size-mb 320 --label BENCH
.\build\zeroext.exe write $Img /model.gguf $HostBin --from-file
```

2. 別ターミナルで対象ドライブをマウントし、`zeroext_bench.exe --read` を実行する（既定 Direct I/O あり / `--no-direct-io` を切り替えて計測）。

```powershell
# マウント側（標準経路 / 既定 Direct I/O を切り替え）
.\build\zeroext.exe mount --source $Img --mountpoint W: --no-direct-io
# または（既定 Direct I/O ON）
.\build\zeroext.exe mount --source $Img --mountpoint W:
```

```powershell
# 計測側
.\build\zeroext_bench.exe --read W:\model.gguf 1024 unbuf seq
.\build\zeroext_bench.exe --read W:\model.gguf 64   unbuf seq
.\build\zeroext_bench.exe --read W:\model.gguf 4    unbuf rnd
.\build\zeroext_bench.exe --read W:\model.gguf 1024 cached seq
```

3. 計測完了後、マウントプロセスを停止（`Ctrl+C`）し、一時ファイル `$HostBin` および `$Img` を削除する。

### 4.4 同一 NVMe SSD 5 方式比較ベンチマーク（`scripts\bench_compare_ssd.ps1`）の実行

Windows Native (NTFS)・`ZeroExt`（`--no-direct-io` / 既定 Direct I/O）・`wsl --mount --vhd`（9P / Linux カーネル参考値）の 5 バックエンドを同一 SSD 上で一括計測する場合は、**管理者権限の PowerShell** で以下を実行する（`Mount-VHD`, `\\.\PhysicalDriveN`, `wsl --mount --vhd` に管理者権限が必要）。

```powershell
powershell -ExecutionPolicy Bypass -File "D:\ZeroExt\scripts\bench_compare_ssd.ps1"
```

計測ログおよび横並びサマリー表は `build\bench_ssd_compare_results.txt` へ出力され、テスト終了時に一時マウント・VHDX・一時ファイルは自動クリーンアップされる（空きドライブ `W:` が使用中の場合は `-MountDrive Y:` 等で変更可能）。
