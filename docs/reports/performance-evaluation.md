# 性能評価レポート

本書は、Read-Only / `--overlay` モードで動作するダイレクト・エクステント I/O 経路（ロックレス extent ベース並列 `pread`、現在は既定で有効・`--no-direct-io` で標準経路へ切替可）の有無による読み取り性能の違いに加え、**同一物理 NVMe SSD 上での Windows Native (NTFS)・`ZeroExt4` (`zeroext4.exe`)・`wsl --mount --vhd` 横並び比較** の実測結果をまとめたものである。

- 対象プロジェクト: **`ZeroExt4`**（コマンド名: `zeroext4.exe`、ベンチマークバイナリ: `zeroext4_bench.exe`、評価リビジョン: `78cd744` / Step 1〜4 最適化・`e2fsprogs 1.47.4` 反映後）
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

## 2. 測定結果（Step 1〜4 最適化・`e2fsprogs 1.47.4` 反映後・`2026-11-21` 再評価）

### 2.1 WinFsp ライブマウント経由（`.img` 上の `model.gguf`）

#### 主測定（256 MB ファイル）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `0.379 s` / `675.8 MB/s` (`676 IOPS`) | `0.092 s` / **`2776.4 MB/s`** (`2776 IOPS`) | **4.11x** |
| `UNBUF SEQ (64KB)` | `0.406 s` / `630.9 MB/s` (`10094 IOPS`) | `0.146 s` / **`1751.9 MB/s`** (`28030 IOPS`) | **2.78x** |
| `UNBUF RND (4KB)` (64 MB / 16K ops) | `0.447 s` / `143.1 MB/s` (`36640 IOPS`) | `0.448 s` / `142.9 MB/s` (`36576 IOPS`) | **1.00x** |
| `CACHED SEQ (1024KB)` (Cold) | `0.413 s` / `619.4 MB/s` (`619 IOPS`) | `0.082 s` / **`3132.1 MB/s`** (`3132 IOPS`) | **5.06x** |

#### 参考（64 MB ファイル条件での実マウント比較・Step 1〜4 時点 `f173d76`）

| ワークロード | `--no-direct-io` (標準経路) | 既定 Direct I/O ON | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `733.1 MB/s` | **`2862.5 MB/s`** | **3.90x** |
| `UNBUF SEQ (64KB)` | `638.9 MB/s` | **`1981.7 MB/s`** | **3.10x** |
| `UNBUF RND (4KB)` | `150.7 MB/s` | **`154.4 MB/s`** | **1.02x** |
| `CACHED SEQ (1024KB)` (Cold) | `1189.1 MB/s` | **`2474.3 MB/s`** | **2.08x** |

256 MB 条件（最新計測）でも一貫して大ブロック・シーケンシャル読み取りにおいて約 2.78x〜5.06x（最大 `3,132.1 MB/s`）の高速化が確認された。

---

### 2.2 VFS 単体層（インプロセス測定、`zeroext4_bench.exe`、`enable_direct_io=0` vs `1`）

対象 extent 構成: `/model.gguf` `runs=1`, `/frag.bin` `runs=65`

| ワークロード | `enable_direct_io=0` (標準経路) | `enable_direct_io=1` (既定 DIO) | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `1T Seq  64KB /model.gguf (512MB)` | `0.600 s` / `853.7 MB/s` (`13659 IOPS`, `73.21 us/op`) | `0.099 s` / **`5164.1 MB/s`** (`82625 IOPS`, `12.10 us/op`) | **6.05x** |
| `1T Seq   1MB /model.gguf (1024MB)` | `1.204 s` / `850.5 MB/s` (`851 IOPS`, `1175.77 us/op`) | `0.148 s` / **`6908.9 MB/s`** (`6909 IOPS`, `144.74 us/op`) | **8.12x** |
| `1T Seq   4KB /model.gguf (128MB)` | `0.156 s` / `820.1 MB/s` (`209944 IOPS`, `4.76 us/op`) | `0.123 s` / **`1037.5 MB/s`** (`265612 IOPS`, `3.76 us/op`) | **1.27x** |
| `1T Rnd   4KB /model.gguf (32K ops)` | `0.199 s` / `643.8 MB/s` (`164820 IOPS`, `6.07 us/op`) | `0.167 s` / **`767.0 MB/s`** (`196347 IOPS`, `5.09 us/op`) | **1.19x** |
| `8T Seq  64KB /model.gguf (2048MB)` | `2.442 s` / `838.7 MB/s` (`13419 IOPS`, `74.52 us/op`) | `0.068 s` / **`30302.9 MB/s`** (`484846 IOPS`, `2.06 us/op`) | **36.13x** |
| `8T Rnd   4KB /model.gguf (131K ops)` | `1.103 s` / `464.2 MB/s` (`118828 IOPS`, `8.42 us/op`) | `0.400 s` / **`1280.4 MB/s`** (`327778 IOPS`, `3.05 us/op`) | **2.76x** |
| `8T Rnd   4KB /frag.bin   (131K ops)` | `1.358 s` / `376.9 MB/s` (`96497 IOPS`, `10.36 us/op`) | `0.409 s` / **`1251.9 MB/s`** (`320487 IOPS`, `3.12 us/op`) | **3.32x** |

<sub>※ `e2fsprogs 1.47.4` 対応において `fs->icache` 等の非スレッドセーフな共有状態を保護するため `ctx->fs` 呼び出しに `fs_lock` を導入した結果、標準経路 (`enable_direct_io=0`) の 8 スレッド並列読み取りは直列化され、8T 4KB ランダムを含め全項目でロックレスな Direct I/O ON が 2.76x〜36.13x 高速となった。</sub>

---

### 2.3 同一 NVMe SSD 5 方式比較（Windows Native NTFS / `ZeroExt4` / `wsl --mount --vhd`）

#### 測定環境および条件（同一 SSD 上での公平比較）

`scripts/bench_compare_ssd.ps1` を管理者 PowerShell で実行し、全バックエンドを同一物理 NVMe SSD（`C:` ドライブ）上で計測した。`ZeroExt4` 側は `.img` ファイルの OS キャッシュ干渉を排除するため `Mount-VHD -ReadOnly` でアタッチした `\\.\PhysicalDrive4`（`win32_io.c` が実 SSD と同じ `FILE_FLAG_NO_BUFFERING` でオープン）を使用し、`wsl --mount --vhd` と同一の 1024 MB 固定サイズ VHDX（GPT Partition 1, ext4, `/model.gguf` 512 MB・`Runs: 4`）を共有した。

| 項目 | 実環境・設定値 |
| :--- | :--- |
| 計測日時 | `2026-11-21 17:45:29 +09:00` |
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
| `Cold/Unbuf Seq 1024KB` | `2,058.5` | `75.2` | **`1,563.7`** | `160.7` | `1,317.6` |
| `Cold/Unbuf Seq 64KB` | `1,074.3` | `75.0` | **`671.6`** | `107.8` | `448.3` |
| `Cold/Unbuf Rnd 4KB` | `55.4` | `34.5` | **`37.3`** | `9.5` | `20.1` |
| `Warm/Cached Seq 1024KB` | `10,552.5` | **`10,346.1`** | **`10,542.5`** | `196.0` | `10,409.7` |
| `Warm/Cached Seq 64KB` | `4,971.2` | **`6,987.7`** | **`8,697.8`** | `120.0` | `14,375.3` |
| `Warm/Cached Rnd 4KB` | `916.3` | **`999.0`** | **`990.5`** | `17.9` | `3,189.4` |

#### 詳細実測値（所要時間 / スループット / IOPS）

<details>
<summary>5 バックエンド全 30 項目の詳細計測ログ（クリックで展開）</summary>

1. **Windows Native (NTFS)**:
   - `UNBUF SEQ (1024KB)`: `0.249 s` / `2058.5 MB/s` (`2059 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.477 s` / `1074.3 MB/s` (`17189 IOPS`)
   - `UNBUF RND (4KB)`: `1.155 s` / `55.4 MB/s` (`14183 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10552.5 MB/s` (`10552 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.103 s` / `4971.2 MB/s` (`79539 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.070 s` / `916.3 MB/s` (`234584 IOPS`)
2. **`ZeroExt4` (`--no-direct-io` 標準経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `6.808 s` / `75.2 MB/s` (`75 IOPS`)
   - `UNBUF SEQ (64KB)`: `6.828 s` / `75.0 MB/s` (`1200 IOPS`)
   - `UNBUF RND (4KB)`: `1.854 s` / `34.5 MB/s` (`8836 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10346.1 MB/s` (`10346 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.073 s` / `6987.7 MB/s` (`111803 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.064 s` / `999.0 MB/s` (`255744 IOPS`)
3. **`ZeroExt4` (Direct I/O ON 既定経路, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.327 s` / `1563.7 MB/s` (`1564 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.762 s` / `671.6 MB/s` (`10746 IOPS`)
   - `UNBUF RND (4KB)`: `1.716 s` / `37.3 MB/s` (`9549 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10542.5 MB/s` (`10542 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.059 s` / `8697.8 MB/s` (`139165 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.065 s` / `990.5 MB/s` (`253557 IOPS`)
4. **`WSL2 9P` (Windows アプリ → `\\wsl.localhost\Ubuntu\mnt\wsl\ext4bench\model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `3.187 s` / `160.7 MB/s` (`161 IOPS`)
   - `UNBUF SEQ (64KB)`: `4.752 s` / `107.8 MB/s` (`1724 IOPS`)
   - `UNBUF RND (4KB)`: `6.764 s` / `9.5 MB/s` (`2422 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `2.612 s` / `196.0 MB/s` (`196 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `4.265 s` / `120.0 MB/s` (`1921 IOPS`)
   - `CACHED-WARM RND (4KB)`: `3.572 s` / `17.9 MB/s` (`4587 IOPS`)
5. **`WSL2 Linux Kernel Direct` (参考値: `/mnt/wsl/ext4bench/model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `0.389 s` / `1317.6 MB/s` (`1318 IOPS`)
   - `UNBUF SEQ (64KB)`: `1.142 s` / `448.3 MB/s` (`7174 IOPS`)
   - `UNBUF RND (4KB)`: `3.182 s` / `20.1 MB/s` (`5148 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10409.7 MB/s` (`10410 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.036 s` / `14375.3 MB/s` (`230004 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.020 s` / `3189.4 MB/s` (`816489 IOPS`)

</details>

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）が大幅に高速化される理由

- **VFS 単体で最大 36.13x（1T 1MB `6,908.9 MB/s` / 8T 64KB `30,302.9 MB/s`）、WinFsp 実マウントでも最大 5.06x（`UNBUF SEQ 1024KB` `2,776.4 MB/s`、Cached Cold `3,132.1 MB/s`）を達成。**
- 標準経路（`--no-direct-io`）は、任意の読込サイズであっても内部で `ext2fs_file_open` / `ext2fs_file_llseek` / `ext2fs_file_read` を介して **4 KB ブロック単位の `cache_lock` 取得・LRU 探索・コピー**を繰り返し実行するため、1 MB 読込 1 回あたり約 256 回のブロック単位オーバーヘッド（`1,175.77 us/op`）が発生する。さらに upstream `e2fsprogs 1.47.4` への更新後は、非スレッドセーフな `fs->icache` 等を保護するため `ctx->fs` 呼び出しが `fs_lock` で直列化されるため、多スレッド時のスループットも単一スレッド相当（`8T Seq 64KB` で `838.7 MB/s`）に頭打ちとなる。
- Direct I/O ON（既定）では、ファイルハンドルまたはシャード化 `extent_cache` に保持した `ext4_extent_map_t` に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、`fs_lock` を一切取得せず `FILE_FLAG_OVERLAPPED` によるロックレス並列 `ReadFile`（`win32_io_pread_direct`）で最大 1 MB まとめて読み切るため、VFS レイテンシが `144.74 us/op`（8T 64KB は `2.06 us/op`）まで短縮される。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **`FILE_FLAG_OVERLAPPED` とロックレス経路による多スレッド 4 KB 性能**:
   - Step 1 の `FILE_FLAG_OVERLAPPED`（[win32_io.c](../../src/io/win32_io.c)）導入により `FileObject->Lock` によるカーネル直列化（Lock Convoy）を解消した結果、Direct I/O 側の `8T Rnd 4KB` は最適化前の `632.4` → **`1,280.4 MB/s` (`327,778 IOPS`)**（`/frag.bin`: `640.6` → **`1,251.9 MB/s`**）と約 2 倍に向上しており、`e2fsprogs 1.47.4` 適用後もロックレスのまま維持されている。
   - 一方、標準経路（`enable_direct_io=0`）は `e2fsprogs 1.47.4` 以降の `fs_lock` 導入により `8T Rnd 4KB` が `464.2 MB/s`（`/frag.bin`: `376.9 MB/s`）となったため、現在では多スレッド 4 KB ランダム読込を含め**全ワークロードで既定の Direct I/O ON が 2.76x〜3.32x 優位**となる。
2. **WinFsp 実マウント時の 4 KB 非バッファランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` はカーネル〜ユーザー空間ディスパッチ費用（約 `25〜28 us/op`）が支配的となるため、`--direct-io` ON/OFF の差は **1.00x**（`142.9` vs `143.1 MB/s`）にとどまる。ただし通常のキャッシュ有効（`CACHED-WARM`）利用では後述の NT Cache Manager により **`990.5 MB/s` (`253,557 IOPS`)** に達する。

### 3.3 同一 SSD 5 方式比較から得られた技術的知見・Step 1〜4 の効果

1. **実ブロックデバイス直読（Cold / Unbuffered）で `ZeroExt4`（Direct I/O 既定）は `WSL2 9P` 比最大 9.73x・`--no-direct-io` 比 20.8x を達成**:
   - `Cold/Unbuf Seq 1024KB`: **`1,563.7 MB/s`**（`--no-direct-io` `75.2 MB/s` の **20.8x**、`WSL2 9P` `160.7 MB/s` の **9.73x**、`WSL2 Linux Kernel` `1,317.6 MB/s` の **1.19x**、`Native NTFS` `2,058.5 MB/s` の **76.0%**）。
   - `Cold/Unbuf Seq 64KB`: **`671.6 MB/s`**（`--no-direct-io` `75.0 MB/s` の **8.95x**、`WSL2 9P` `107.8 MB/s` の **6.23x**、`WSL2 Linux Kernel` `448.3 MB/s` の **1.50x**）。
   - `Cold/Unbuf Rnd 4KB`: **`37.3 MB/s` (`9,549 IOPS`)**（`WSL2 9P` `9.5 MB/s` の **3.93x**、`WSL2 Linux Kernel` `20.1 MB/s` の **1.86x**）。
   - 実ディスク `\\.\PhysicalDriveN` はセクタ整合のため `FILE_FLAG_NO_BUFFERING` で開く必要があり、`--no-direct-io` 標準経路では 1 MB ごとに 256 回の 4 KB 読込が発生して `75.2 MB/s` が上限となるため、RO / `--overlay` での Direct I/O 既定有効化が実ディスク性能を支えている。
2. **Warm / Cached で `ZeroExt4` が `Native NTFS` 同等以上・`WSL2 9P` 比 53x〜72x に到達（Step 2 の効果）**:
   - Step 2（`8bd61df`）で Read-Only マウント時の `FileInfoTimeout` 既定値を `(UINT32)-1` (`INFINITE`) としたことで、Windows NT Cache Manager（`CcCopyRead`）が有効化され、以前の課題だった Warm 時の頭打ち（`1,490.5 MB/s` / `36.8 MB/s`）が完全に解消された：
     - `Warm/Cached Seq 1024KB`: **`10,542.5 MB/s`**（`Native NTFS` `10,552.5 MB/s` の **1.00x**、`WSL2 9P` `196.0 MB/s` の **53.8x**、改善前 `1,490.5 MB/s` 比 **7.07x**）
     - `Warm/Cached Seq 64KB`: **`8,697.8 MB/s`**（`Native NTFS` `4,971.2 MB/s` の **1.75x**、`WSL2 9P` `120.0 MB/s` の **72.5x**、改善前 `661.2 MB/s` 比 **13.2x**）
     - `Warm/Cached Rnd 4KB`: **`990.5 MB/s` (`253,557 IOPS`)**（`Native NTFS` `916.3 MB/s` の **1.08x**、`WSL2 9P` `17.9 MB/s` の **55.3x**、改善前 `36.8 MB/s` 比 **26.9x**）
   - `--no-direct-io` 側でも 2 周目以降は OS キャッシュで処理されるため `10,346.1 / 6,987.7 / 999.0 MB/s` となるが、初回読込（Cold）が **`20.8x`** 速い既定の Direct I/O ON が総合的に優位である。
3. **`WSL2 9P`（`\\wsl.localhost`）経由との比較**:
   - WSL2 内の Linux カーネル直接読み取りは Warm 時に `10,409.7〜14,375.3 MB/s`（4 KB ランダム `3,189.4 MB/s`）に達するものの、Windows 側アプリから `\\wsl.localhost` 越しにアクセスする場合は 9P / HVSocket 往復が律速となり、`196.0 MB/s`（4 KB ランダムは `17.9 MB/s`）が上限となる。`ZeroExt4` は Windows ネイティブのドライブレター経由で **Cold 時最大 9.73x・Warm 時 53x〜72x** 高速にアクセスできる。

### 3.4 実運用における使い分け指針

- **既定設定（RO / `--overlay` で Direct I/O 自動有効 + `FileInfoTimeout = INFINITE`）**:
  - 初回（Cold）の大容量・実ディスク直読（`1,563.7 MB/s`）から 2 回目以降のメモリ帯域（`10.5 GB/s` / `253K IOPS`）、さらに Step 4 の Dentry キャッシュ・`FspGetDirInfoByName` による高速な Explorer / シェルメタデータ応答までそのまま最高性能を発揮する。
- **`--no-direct-io` / `--file-info-timeout <ms>` を検討すべきケース**:
  - 互換性確認やトラブルシューティング目的で標準 `ext2fs_file_read` 経路にフォールバックさせたい場合は `--no-direct-io`（`fs_lock` により多スレッド並列時は直列化される）。
  - 外部プロセスからイメージを同時更新するような特殊用途でカーネルキャッシュを抑制したい場合は `--file-info-timeout 1000`（`--rw` および書き込み可能 `--overlay` は既定で `1000 ms` かつ Direct I/O 無効）。

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
