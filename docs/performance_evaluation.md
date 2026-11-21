# 性能評価報告書 (Direct I/O 比較 / Windows NTFS・WSL2 同一 SSD 比較)

本書は、Read-Only / `--overlay` モードで動作するダイレクト・エクステント I/O 経路（ロックレス extent ベース並列 `pread`、現在は既定で有効・`--no-direct-io` で標準経路へ切替可）の有無による読み取り性能の違いに加え、**同一物理 NVMe SSD 上での Windows Native (NTFS)・`ZeroExt` (`zeroext.exe`)・`wsl --mount --vhd` 横並び比較** の実測結果をまとめたものである。

- 対象プロジェクト: **`ZeroExt`**（コマンド名: `zeroext.exe`、ベンチマークバイナリ: `zeroext_bench.exe`、評価リビジョン: `f173d76` / Step 1〜4 最適化反映後）
- ベンチマーク実装: [tests/bench_direct_io.c](../tests/bench_direct_io.c)（ビルド成果物: `build/zeroext_bench.exe`）
- 同一 SSD 比較スクリプト: [scripts/bench_compare_ssd.ps1](../scripts/bench_compare_ssd.ps1)（管理者権限 PowerShell 実行）
- 最適化の経緯と詳細トレース解析: [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md)
- 注記: 本書内の「`--no-direct-io` / `Std`」は標準 `ext2fs_file_read` 経路、「`--direct-io` ON / `DIO`」は RO / `--overlay` 時の既定動作に対応する。また Step 2 (`8bd61df`) 以降、RO マウント時は `FileInfoTimeout = INFINITE` (`(UINT32)-1`) が既定となり Windows NT Cache Manager が有効化されている。

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

## 2. 測定結果（Step 1〜4 最適化後・`2026-11-21` 再評価）

### 2.1 WinFsp ライブマウント経由（`.img` 上の `model.gguf`）

#### 主測定（256 MB ファイル）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `0.351 s` / `729.0 MB/s` (`729 IOPS`) | `0.090 s` / **`2848.9 MB/s`** (`2849 IOPS`) | **3.91x** |
| `UNBUF SEQ (64KB)` | `0.405 s` / `632.0 MB/s` (`10112 IOPS`) | `0.127 s` / **`2012.4 MB/s`** (`32199 IOPS`) | **3.18x** |
| `UNBUF RND (4KB)` (64 MB / 16K ops) | `0.465 s` / `137.6 MB/s` (`35215 IOPS`) | `0.415 s` / **`154.2 MB/s`** (`39472 IOPS`) | **1.12x** |
| `CACHED SEQ (1024KB)` (Cold) | `0.224 s` / `1142.6 MB/s` (`1143 IOPS`) | `0.081 s` / **`3176.5 MB/s`** (`3177 IOPS`) | **2.78x** |

#### 参考（64 MB ファイル条件での実マウント比較）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `733.1 MB/s` | **`2862.5 MB/s`** | **3.90x** |
| `UNBUF SEQ (64KB)` | `638.9 MB/s` | **`1981.7 MB/s`** | **3.10x** |
| `UNBUF RND (4KB)` | `150.7 MB/s` | **`154.4 MB/s`** | **1.02x** |
| `CACHED SEQ (1024KB)` (Cold) | `1189.1 MB/s` | **`2474.3 MB/s`** | **2.08x** |

64 MB / 256 MB の両条件で一貫して大ブロック・シーケンシャル読み取りにおいて約 3.1x〜3.9x（最大 `3,176.5 MB/s`）の高速化が確認された。

---

### 2.2 VFS 単体層（インプロセス測定、`zeroext_bench.exe`、`enable_direct_io=0` vs `1`）

対象 extent 構成: `/model.gguf` `runs=1`, `/frag.bin` `runs=65`

| ワークロード | `enable_direct_io=0` (標準経路) | `enable_direct_io=1` (既定 DIO) | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `1T Seq  64KB /model.gguf (512MB)` | `0.568 s` / `902.0 MB/s` (`14433 IOPS`, `69.29 us/op`) | `0.091 s` / **`5615.7 MB/s`** (`89851 IOPS`, `11.13 us/op`) | **6.23x** |
| `1T Seq   1MB /model.gguf (1024MB)` | `1.113 s` / `920.2 MB/s` (`920 IOPS`, `1086.72 us/op`) | `0.134 s` / **`7643.4 MB/s`** (`7643 IOPS`, `130.83 us/op`) | **8.31x** |
| `1T Seq   4KB /model.gguf (128MB)` | `0.186 s` / `687.7 MB/s` (`176062 IOPS`, `5.68 us/op`) | `0.119 s` / **`1077.2 MB/s`** (`275750 IOPS`, `3.63 us/op`) | **1.57x** |
| `1T Rnd   4KB /model.gguf (32K ops)` | `0.214 s` / `596.8 MB/s` (`152781 IOPS`, `6.55 us/op`) | `0.161 s` / **`794.2 MB/s`** (`203323 IOPS`, `4.92 us/op`) | **1.33x** |
| `8T Seq  64KB /model.gguf (2048MB)` | `0.351 s` / `5834.4 MB/s` (`93350 IOPS`, `10.71 us/op`) | `0.062 s` / **`33225.9 MB/s`** (`531615 IOPS`, `1.88 us/op`) | **5.69x** |
| `8T Rnd   4KB /model.gguf (131K ops)` | `0.297 s` / **`1725.9 MB/s`** (`441820 IOPS`, `2.26 us/op`) | `0.411 s` / `1244.9 MB/s` (`318694 IOPS`, `3.14 us/op`) | **0.72x** |
| `8T Rnd   4KB /frag.bin   (131K ops)` | `0.373 s` / **`1374.4 MB/s`** (`351853 IOPS`, `2.84 us/op`) | `0.406 s` / `1260.4 MB/s` (`322654 IOPS`, `3.10 us/op`) | **0.92x** |

<sub>※ 3 回連続実行時の中央値は `8T Seq 64KB DIO = 33,066.3 MB/s`、`8T Rnd 4KB /model.gguf DIO = 1,237.7 MB/s`、`8T Rnd 4KB /frag.bin DIO = 1,245.3 MB/s`。</sub>

---

### 2.3 同一 NVMe SSD 5 方式比較（Windows Native NTFS / `ZeroExt` / `wsl --mount --vhd`）

#### 測定環境および条件（同一 SSD 上での公平比較）

`scripts/bench_compare_ssd.ps1` を管理者 PowerShell で実行し、全バックエンドを同一物理 NVMe SSD（`C:` ドライブ）上で計測した。`ZeroExt` 側は `.img` ファイルの OS キャッシュ干渉を排除するため `Mount-VHD -ReadOnly` でアタッチした `\\.\PhysicalDrive4`（`win32_io.c` が実 SSD と同じ `FILE_FLAG_NO_BUFFERING` でオープン）を使用し、`wsl --mount --vhd` と同一の 1024 MB 固定サイズ VHDX（GPT Partition 1, ext4, `/model.gguf` 512 MB・`Runs: 4`）を共有した。

| 項目 | 実環境・設定値 |
| :--- | :--- |
| 計測日時 | `2026-11-21 13:09:17 +09:00` |
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
| `Cold/Unbuf Seq 1024KB` | `2,110.9` | `64.0` | **`1,550.6`** | `167.5` | `1,729.8` |
| `Cold/Unbuf Seq 64KB` | `1,098.6` | `67.3` | **`665.6`** | `108.2` | `433.3` |
| `Cold/Unbuf Rnd 4KB` | `55.6` | `34.0` | **`37.1`** | `9.7` | `23.3` |
| `Warm/Cached Seq 1024KB` | `10,414.4` | **`10,931.5`** | **`10,523.6`** | `198.4` | `9,425.0` |
| `Warm/Cached Seq 64KB` | `8,357.1` | **`8,829.2`** | **`8,672.1`** | `125.1` | `12,400.0` |
| `Warm/Cached Rnd 4KB` | `979.5` | **`1,020.4`** | **`1,017.4`** | `17.6` | `3,040.7` |

#### 詳細実測値（所要時間 / スループット / IOPS）

<details>
<summary>5 バックエンド全 30 項目の詳細計測ログ（クリックで展開）</summary>

1. **Windows Native (NTFS)**:
   - `UNBUF SEQ (1024KB)`: `0.243 s` / `2110.9 MB/s` (`2111 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.466 s` / `1098.6 MB/s` (`17577 IOPS`)
   - `UNBUF RND (4KB)`: `1.151 s` / `55.6 MB/s` (`14232 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10414.4 MB/s` (`10414 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.061 s` / `8357.1 MB/s` (`133714 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.065 s` / `979.5 MB/s` (`250749 IOPS`)
2. **`ZeroExt` (`--no-direct-io` 標準経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `8.002 s` / `64.0 MB/s` (`64 IOPS`)
   - `UNBUF SEQ (64KB)`: `7.610 s` / `67.3 MB/s` (`1076 IOPS`)
   - `UNBUF RND (4KB)`: `1.882 s` / `34.0 MB/s` (`8708 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.047 s` / `10931.5 MB/s` (`10931 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.058 s` / `8829.2 MB/s` (`141267 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.063 s` / `1020.4 MB/s` (`261229 IOPS`)
3. **`ZeroExt` (Direct I/O ON 既定経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.330 s` / `1550.6 MB/s` (`1551 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.769 s` / `665.6 MB/s` (`10650 IOPS`)
   - `UNBUF RND (4KB)`: `1.726 s` / `37.1 MB/s` (`9491 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10523.6 MB/s` (`10524 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.059 s` / `8672.1 MB/s` (`138754 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.063 s` / `1017.4 MB/s` (`260454 IOPS`)
4. **`WSL2 9P` (Windows アプリ → `\\wsl.localhost\Ubuntu\mnt\wsl\ext4bench\model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `3.056 s` / `167.5 MB/s` (`168 IOPS`)
   - `UNBUF SEQ (64KB)`: `4.732 s` / `108.2 MB/s` (`1731 IOPS`)
   - `UNBUF RND (4KB)`: `6.593 s` / `9.7 MB/s` (`2485 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `2.580 s` / `198.4 MB/s` (`198 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `4.093 s` / `125.1 MB/s` (`2002 IOPS`)
   - `CACHED-WARM RND (4KB)`: `3.644 s` / `17.6 MB/s` (`4496 IOPS`)
5. **`WSL2 Linux Kernel Direct` (参考値: `/mnt/wsl/ext4bench/model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `0.296 s` / `1729.8 MB/s` (`1730 IOPS`)
   - `UNBUF SEQ (64KB)`: `1.182 s` / `433.3 MB/s` (`6932 IOPS`)
   - `UNBUF RND (4KB)`: `2.748 s` / `23.3 MB/s` (`5963 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.054 s` / `9425.0 MB/s` (`9425 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.041 s` / `12400.0 MB/s` (`198400 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.021 s` / `3040.7 MB/s` (`778420 IOPS`)

</details>

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）が大幅に高速化される理由

- **VFS 単体で最大 8.31x（1T 1MB `7,643.4 MB/s` / 8T 64KB `33,225.9 MB/s`）、WinFsp 実マウントでも最大 3.91x（`2,848.9 MB/s`、Cached Cold `3,176.5 MB/s`）を達成。**
- 標準経路（`--no-direct-io`）は、任意の読込サイズであっても内部で `ext2fs_file_open` / `ext2fs_file_llseek` / `ext2fs_file_read` を介して **4 KB ブロック単位の `cache_lock` 取得・LRU 探索・コピー**を繰り返し実行するため、1 MB 読込 1 回あたり約 256 回のブロック単位オーバーヘッド（`1,086.72 us/op`）が発生する。
- Direct I/O ON（既定）では、ファイルハンドルまたはシャード化 `extent_cache` に保持した `ext4_extent_map_t` に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、`FILE_FLAG_OVERLAPPED` によるロックレス並列 `ReadFile`（`win32_io_pread_direct`）で最大 1 MB まとめて読み切るため、VFS レイテンシが `130.83 us/op`（8T 64KB は `1.88 us/op`）まで短縮される。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **`FILE_FLAG_OVERLAPPED` とキャッシュシャード化による多スレッド 4 KB 性能の大幅改善**:
   - Step 1 の `FILE_FLAG_OVERLAPPED`（[win32_io.c](../src/io/win32_io.c)）導入により `FileObject->Lock` によるカーネル直列化（Lock Convoy）を解消した結果、Direct I/O 側の `8T Rnd 4KB` は最適化前の `632.4` → **`1,244.9 MB/s`**（`/frag.bin`: `640.6` → **`1,260.4 MB/s`**）と約 2 倍に向上した。
   - 一方、標準経路（`enable_direct_io=0`）でも Step 3 の 32 シャード＋Shared ロック化により `win32_io.cache_lock` 競合を 94〜96% 削減したため、`8T Rnd 4KB` は `1,000.4` → **`1,725.9 MB/s` (`441,820 IOPS`)**（`/frag.bin`: `812.1` → **`1,374.4 MB/s`**）へ伸長した。作業集合の 12.5〜25% が 8 MB ユーザー空間キャッシュに収まるインプロセス高並列 4 KB 条件では、システムコールを省ける標準経路が引き続きやや優位（`0.72x〜0.92x`）となる。
2. **WinFsp 実マウント時の 4 KB 非バッファランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` はカーネル〜ユーザー空間ディスパッチ費用（約 `25〜28 us/op`）が支配的となるため、`--direct-io` ON/OFF の差は **1.12x**（`154.2` vs `137.6 MB/s`）にとどまる。ただし通常のキャッシュ有効（`CACHED-WARM`）利用では後述の NT Cache Manager により **`1,017.4 MB/s` (`260,454 IOPS`)** に達する。

### 3.3 同一 SSD 5 方式比較から得られた技術的知見・Step 1〜4 の効果

1. **実ブロックデバイス直読（Cold / Unbuffered）で `ZeroExt`（Direct I/O 既定）は `WSL2 9P` 比最大 9.26x・`--no-direct-io` 比 24.2x を達成**:
   - `Cold/Unbuf Seq 1024KB`: **`1,550.6 MB/s`**（`--no-direct-io` `64.0 MB/s` の **24.2x**、`WSL2 9P` `167.5 MB/s` の **9.26x**、`WSL2 Linux Kernel` `1,729.8 MB/s` の **89.6%**、`Native NTFS` `2,110.9 MB/s` の **73.5%**）。
   - `Cold/Unbuf Seq 64KB`: **`665.6 MB/s`**（`--no-direct-io` の **9.89x**、`WSL2 9P` の **6.15x**、`WSL2 Linux Kernel` `433.3 MB/s` の **1.54x**）。
   - `Cold/Unbuf Rnd 4KB`: **`37.1 MB/s` (`9,491 IOPS`)**（`WSL2 9P` `9.7 MB/s` の **3.82x**、`WSL2 Linux Kernel` `23.3 MB/s` の **1.59x**）。
   - 実ディスク `\\.\PhysicalDriveN` はセクタ整合のため `FILE_FLAG_NO_BUFFERING` で開く必要があり、`--no-direct-io` 標準経路では 1 MB ごとに 256 回の 4 KB 読込が発生して `64.0 MB/s` が上限となるため、RO / `--overlay` での Direct I/O 既定有効化が実ディスク性能を支えている。
2. **Warm / Cached で `ZeroExt` が `Native NTFS` 同等以上・`WSL2 9P` 比 53x〜69x に到達（Step 2 の効果）**:
   - Step 2（`8bd61df`）で Read-Only マウント時の `FileInfoTimeout` 既定値を `(UINT32)-1` (`INFINITE`) としたことで、Windows NT Cache Manager（`CcCopyRead`）が有効化され、以前の課題だった Warm 時の頭打ち（`1,490.5 MB/s` / `36.8 MB/s`）が完全に解消された：
     - `Warm/Cached Seq 1024KB`: **`10,523.6 MB/s`**（`Native NTFS` `10,414.4 MB/s` の **1.01x**、`WSL2 9P` `198.4 MB/s` の **53.0x**、改善前 `1,490.5 MB/s` 比 **7.06x**）
     - `Warm/Cached Seq 64KB`: **`8,672.1 MB/s`**（`Native NTFS` `8,357.1 MB/s` の **1.04x**、`WSL2 9P` `125.1 MB/s` の **69.3x**、改善前 `661.2 MB/s` 比 **13.1x**）
     - `Warm/Cached Rnd 4KB`: **`1,017.4 MB/s` (`260,454 IOPS`)**（`Native NTFS` `979.5 MB/s` の **1.04x**、`WSL2 9P` `17.6 MB/s` の **57.8x**、改善前 `36.8 MB/s` 比 **27.6x**）
   - `--no-direct-io` 側でも 2 周目以降は OS キャッシュで処理されるため `10,931.5 / 8,829.2 / 1,020.4 MB/s` となるが、初回読込（Cold）が **`24.2x`** 速い既定の Direct I/O ON が総合的に優位である。
3. **`WSL2 9P`（`\\wsl.localhost`）経由との比較**:
   - WSL2 内の Linux カーネル直接読み取りは Warm 時に `9,425〜12,400 MB/s`（4 KB ランダム `3,040.7 MB/s`）に達するものの、Windows 側アプリから `\\wsl.localhost` 越しにアクセスする場合は 9P / HVSocket 往復が律速となり、`198.4 MB/s`（4 KB ランダムは `17.6 MB/s`）が上限となる。`ZeroExt` は Windows ネイティブのドライブレター経由で **Cold 時最大 9.26x・Warm 時 53x〜69x** 高速にアクセスできる。

### 3.4 実運用における使い分け指針

- **既定設定（RO / `--overlay` で Direct I/O 自動有効 + `FileInfoTimeout = INFINITE`）**:
  - 初回（Cold）の大容量・実ディスク直読（`1,550.6 MB/s`）から 2 回目以降のメモリ帯域（`10.5 GB/s` / `260K IOPS`）、さらに Step 4 の Dentry キャッシュ・`FspGetDirInfoByName` による高速な Explorer / シェルメタデータ応答までそのまま最高性能を発揮する。
- **`--no-direct-io` / `--file-info-timeout <ms>` を検討すべきケース**:
  - `.img` ファイルや 8 MB ユーザー空間 LRU キャッシュ内に高密度に収まる特殊な多スレッド 4 KB ランダム読込をインプロセス・非キャッシュ用途で優先したい場合は `--no-direct-io`。
  - 外部プロセスからイメージを同時更新するような特殊用途でカーネルキャッシュを抑制したい場合は `--file-info-timeout 1000`（`--rw` および書き込み可能 `--overlay` は既定で `1000 ms` かつ Direct I/O 無効）。

> [!NOTE]
> Step 1〜4 の各段階における Perfetto トレース定量的効果およびメタデータ最適化（`FspGetSecurityByName` **11.97x**、`FspOpen` **27.62x**、`FspReadDirectory` **14.51x**）の詳細は [Perfetto トレース定量解析・性能改善計画](perfetto_trace_analysis.md) を参照。

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
