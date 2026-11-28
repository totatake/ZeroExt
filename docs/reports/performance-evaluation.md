# 性能評価レポート

本書は、**`ZeroExt4` (`zeroext4.exe` / `zeroext4_bench.exe`) の全 I/O モード比較**（Read-Only 既定のダイレクト・エクステント I/O、`--no-direct-io` 標準経路、`--rw` 既定経路、オプトイン `--rw --rw-fast-io` 経路）および **同一物理 NVMe SSD 上での Windows Native (NTFS)・`ZeroExt4`・`wsl --mount --vhd` 横並び比較** の実測結果をまとめたものである。

- 対象プロジェクト: **`ZeroExt4`**（コマンド名: `zeroext4.exe`、ベンチマークバイナリ: `zeroext4_bench.exe`、評価バージョン: `v0.11.0` / Phase 1〜3 完了時点、`2026-11-28` 全項目再計測）
- ベンチマーク実装: [tests/bench_direct_io.c](../../tests/bench_direct_io.c)（ビルド成果物: `build/zeroext4_bench.exe`）
- 同一 SSD 比較スクリプト: [scripts/bench_compare_ssd.ps1](../../scripts/bench_compare_ssd.ps1)（管理者権限 PowerShell 実行）
- 最適化の経緯と詳細トレース解析: [Perfetto トレース解析・最適化レポート](perfetto-trace-analysis.md)

### 本書における測定モードの定義（`--rw-fast-io` 使用有無の明確化）

`v0.10.0` ではバッチ連続エクステント読込を一時的に標準経路へ組み込んでいたため、`v0.10.0` 当時の計測では `--no-direct-io` 側にもその効果が含まれていた。`v0.10.1` 以降では安全性と責務分離のため、バッチ連続エクステント読込／書込を **`--rw --rw-fast-io` オプトイン専用** へ分離し、RO `--no-direct-io` および既定 `--rw` は上流 `e2fsprogs` (`ext2fs_file_read`/`write`) 準拠へ戻している。本書では誤認を防ぐため、最新コードで再計測のうえ以下の 4 モードを厳密に区別して表記する。

| 表記 / CLI フラグ | モード | 内部 I/O 経路 | `--rw-fast-io` | WinFsp `FileInfoTimeout` 既定 |
| :--- | :--- | :--- | :---: | :--- |
| **`RO 既定 (Direct I/O ON)`** | Read-Only / `--overlay` 既定 | ロックレス `cached_extents` + `win32_io_pread_direct` | **不使用**（指定不可 `-EINVAL`） | `INFINITE` (`CcCopyRead` 有効) |
| **`RO --no-direct-io` (`Std`)** | Read-Only 標準フォールバック | 上流 `libext2fs` (`ext2fs_file_read` 4 KB 単位) | **不使用** | `INFINITE` (`CcCopyRead` 有効) |
| **`--rw 既定` (`e2fsprogs`)** | Read-Write 既定 | 上流 `libext2fs` (`ext2fs_file_read`/`write` ステートレス) | **不使用** | `1000 ms` |
| **`--rw --rw-fast-io`** | Read-Write 実験的高速化 | `fs_lock` 外並列読込＋バッチ連続エクステント書込 | **使用**（オプトイン） | `1000 ms` |

---

## 1. 評価環境とワークロード構成

### 1.1 ビルド・ソフトウェア構成

| 項目 | 設定値 |
| :--- | :--- |
| ビルド構成 | CMake (`Ninja`) + MSVC 19.44 / `Release` ビルド (`scripts/build_and_test.ps1`) |
| ユーザー空間ブロックキャッシュ | `--cache-blocks 2048`（4 KB ブロック × 2048 = 8 MB LRU キャッシュ、デフォルト設定） |
| バッキングストレージ | Windows ローカルファイル上の ext4 イメージファイル（`.img`） |
| WinFsp マウントパス | `W:`（`zeroext4.exe mount --source <img_path> --mountpoint W: [--no-direct-io \| --rw \| --rw --rw-fast-io]`） |

### 1.2 テストデータセット

1. **実マウント（WinFsp 経由）評価**:
   - **測定対象**: 320 MB ext4 イメージ上に配置した **256 MB** 連続ファイル `W:\model.gguf`（LLM モデル等のシーケンシャル / `mmap` 想定ワークロード）。
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

## 2. 測定結果（`v0.11.0` / Phase 1〜3 完了時点・`2026-11-28` 再計測）

### 2.1 WinFsp ライブマウント経由（256 MB `.img` 上の `model.gguf`・4 モード実測比較）

320 MB ext4 イメージ上の 256 MB 連続ファイル `W:\model.gguf` に対し、`zeroext4.exe mount` の全 4 モード（RO `--no-direct-io` / RO 既定 Direct I/O ON / `--rw` 既定 / `--rw --rw-fast-io`）を同一条件で実測した結果は以下の通り。

| ワークロード | RO `--no-direct-io`<br><sub>(`e2fsprogs` / `fast-io` なし)</sub> | RO 既定<br><sub>(Direct I/O ON / `fast-io` なし)</sub> | `--rw` 既定<br><sub>(`e2fsprogs` / `fast-io` なし)</sub> | `--rw --rw-fast-io`<br><sub>(バッチ連続 I/O)</sub> |
| :--- | ---: | ---: | ---: | ---: |
| `UNBUF SEQ (1024KB)` | `0.383 s` / `668.9 MB/s`<br><sub>(`669 IOPS`)</sub> | `0.095 s` / **`2,698.7 MB/s`**<br><sub>(`2699 IOPS` / RO Std 比 **4.03x**)</sub> | `0.375 s` / `681.9 MB/s`<br><sub>(`682 IOPS`)</sub> | `0.097 s` / **`2,636.5 MB/s`**<br><sub>(`2636 IOPS` / RW 既定比 **3.87x**)</sub> |
| `UNBUF SEQ (64KB)` | `0.413 s` / `619.2 MB/s`<br><sub>(`9907 IOPS`)</sub> | `0.139 s` / **`1,837.9 MB/s`**<br><sub>(`29407 IOPS` / RO Std 比 **2.97x**)</sub> | `0.421 s` / `608.7 MB/s`<br><sub>(`9739 IOPS`)</sub> | `0.148 s` / **`1,729.3 MB/s`**<br><sub>(`27669 IOPS` / RW 既定比 **2.84x**)</sub> |
| `UNBUF RND (4KB)`<br><sub>(64 MB / 16K ops)</sub> | `0.453 s` / `141.3 MB/s`<br><sub>(`36164 IOPS`)</sub> | `0.430 s` / **`148.9 MB/s`**<br><sub>(`38124 IOPS` / RO Std 比 **1.05x**)</sub> | `0.465 s` / `137.5 MB/s`<br><sub>(`35198 IOPS`)</sub> | `0.487 s` / `131.4 MB/s`<br><sub>(`33644 IOPS` / RW 既定比 0.96x)</sub> |
| `CACHED SEQ (1024KB)`<br><sub>(Cold)</sub> | `0.408 s` / `627.3 MB/s`<br><sub>(`627 IOPS`)</sub> | `0.073 s` / **`3,499.0 MB/s`**<br><sub>(`3499 IOPS` / RO Std 比 **5.58x**)</sub> | `0.379 s` / `676.0 MB/s`<br><sub>(`676 IOPS`)</sub> | `0.105 s` / **`2,431.4 MB/s`**<br><sub>(`2431 IOPS` / RW 既定比 **3.60x**)</sub> |

- **ポイント**:
  - `RO --no-direct-io` と `--rw` 既定はいずれも上流 `ext2fs_file_read` 経路（`--rw-fast-io` 無効）を通るため、`UNBUF SEQ (1024KB)` は **`668.9〜681.9 MB/s`** と同水準になる。
  - **RO 既定（Direct I/O ON, `--rw-fast-io` 不使用）** は `UNBUF SEQ (1024KB)` で **`2,698.7 MB/s`（4.03x）**、`CACHED SEQ (1024KB)`（Cold）で **`3,499.0 MB/s`（5.58x）** と全モード中最速を記録する。
  - **`--rw --rw-fast-io`** をオプトインした場合は、`UNBUF SEQ (1024KB)` で **`2,636.5 MB/s`（既定 `--rw` 比 3.87x）**、`CACHED SEQ (1024KB)`（Cold）で **`2,431.4 MB/s`（3.60x）** へ高速化される（`v0.10.0` 当時に `--no-direct-io` 側で `2,739.8 MB/s` と計測されていたのは、当時の標準経路にこのバッチ並列読込が組み込まれていたためである）。

---

### 2.2 VFS 単体層（インプロセス測定、`zeroext4_bench.exe`、`2026-11-28` 再計測）

#### (A) Read-Only 経路比較：`--no-direct-io` (`enable_direct_io=0`) vs 既定 Direct I/O ON (`enable_direct_io=1`)

※ 両列とも RO マウントであり、`--rw-fast-io` は**不使用**（`zeroext4_bench.exe` 引数なし実行）。対象 extent 構成: `/model.gguf` `runs=1`, `/frag.bin` `runs=65`。

| ワークロード | `enable_direct_io=0`<br><sub>(`--no-direct-io` / 上流 `ext2fs_file_read`)</sub> | `enable_direct_io=1`<br><sub>(RO 既定 Direct I/O ON)</sub> | 速度比 (DIO / Std) |
| :--- | ---: | ---: | ---: |
| `1T Seq  64KB /model.gguf (512MB)` | `0.598 s` / `856.1 MB/s` (`13698 IOPS`, `73.00 us/op`) | `0.098 s` / **`5,248.4 MB/s`** (`83975 IOPS`, `11.91 us/op`) | **6.13x** |
| `1T Seq   1MB /model.gguf (1024MB)` | `1.209 s` / `846.7 MB/s` (`847 IOPS`, `1181.07 us/op`) | `0.146 s` / **`7,018.4 MB/s`** (`7018 IOPS`, `142.48 us/op`) | **8.29x** |
| `1T Seq   4KB /model.gguf (128MB)` | `0.156 s` / `818.3 MB/s` (`209491 IOPS`, `4.77 us/op`) | `0.120 s` / **`1,063.6 MB/s`** (`272290 IOPS`, `3.67 us/op`) | **1.30x** |
| `1T Rnd   4KB /model.gguf (32K ops)` | `0.206 s` / `622.8 MB/s` (`159447 IOPS`, `6.27 us/op`) | `0.176 s` / **`726.9 MB/s`** (`186077 IOPS`, `5.37 us/op`) | **1.17x** |
| `8T Seq  64KB /model.gguf (2048MB)` | `2.402 s` / `852.5 MB/s` (`13640 IOPS`, `73.31 us/op`) | `0.067 s` / **`30,573.7 MB/s`** (`489180 IOPS`, `2.04 us/op`) | **35.86x** |
| `8T Rnd   4KB /model.gguf (131K ops)` | `1.150 s` / `445.4 MB/s` (`114019 IOPS`, `8.77 us/op`) | `0.448 s` / **`1,142.5 MB/s`** (`292468 IOPS`, `3.42 us/op`) | **2.57x** |
| `8T Rnd   4KB /frag.bin   (131K ops)` | `1.392 s` / `367.9 MB/s` (`94187 IOPS`, `10.62 us/op`) | `0.431 s` / **`1,189.3 MB/s`** (`304464 IOPS`, `3.28 us/op`) | **3.23x** |

#### (B) `--rw` モード比較：既定 `e2fsprogs` vs オプトイン `--rw --rw-fast-io` (`zeroext4_bench.exe --rw-bench`)

256 MB テスト用 ext4 イメージ上で、既定の `--rw`（上流 `ext2fs_file_read`/`write` 準拠・`--rw-fast-io` 不使用）と、オプトインの `--rw --rw-fast-io`（バッチ連続エクステント書込・`fs_lock` 外並列読込）を横並び計測した結果は以下の通り（`2026-11-28` 実測）。

| ワークロード | `--rw` 既定 (`e2fsprogs` 準拠)<br><sub>(`--rw-fast-io` 不使用)</sub> | `--rw --rw-fast-io` (オプトイン)<br><sub>(バッチ連続 I/O 使用)</sub> | 速度比 (`fast-io` / 既定) |
| :--- | ---: | ---: | ---: |
| `RW 1T Write Seq  1MB (64MB)`（新規割当） | `0.274 s` / `233.9 MB/s` (`234 IOPS`, `4274.46 us/op`) | `0.017 s` / **`3,714.0 MB/s`** (`3714 IOPS`, `269.25 us/op`) | **15.88x** |
| `RW 1T Write Seq 64KB (64MB)`（新規割当） | `0.287 s` / `222.9 MB/s` (`3567 IOPS`, `280.36 us/op`) | `0.037 s` / **`1,729.5 MB/s`** (`27672 IOPS`, `36.14 us/op`) | **7.76x** |
| `RW 1T Overwrite 64KB (64MB)`（上書更新） | `0.081 s` / `791.9 MB/s` (`12670 IOPS`, `78.92 us/op`) | `0.024 s` / **`2,659.0 MB/s`** (`42544 IOPS`, `23.51 us/op`) | **3.36x** |
| `RW 1T Write Seq  4KB (16MB)`（EOF 追記） | `0.098 s` / `163.5 MB/s` (`41844 IOPS`, `23.90 us/op`) | `0.055 s` / **`291.7 MB/s`** (`74664 IOPS`, `13.39 us/op`) | **1.78x** |
| `RW 1T Read  Seq  1MB (256MB)` | `0.277 s` / `923.2 MB/s` (`923 IOPS`, `1083.15 us/op`) | `0.033 s` / **`7,712.1 MB/s`** (`7712 IOPS`, `129.67 us/op`) | **8.35x** |
| `RW 8T Read  Seq 64KB (1024MB)` | `1.160 s` / `882.5 MB/s` (`14119 IOPS`, `70.82 us/op`) | `0.035 s` / **`29,597.4 MB/s`** (`473559 IOPS`, `2.11 us/op`) | **33.54x** |
| `RW 8T Read  Rnd  4KB (65K ops)` | `0.499 s` / `513.4 MB/s` (`131427 IOPS`, `7.61 us/op`) | `0.132 s` / **`1,942.0 MB/s`** (`497141 IOPS`, `2.01 us/op`) | **3.78x** |

---

### 2.3 同一 NVMe SSD 5 方式比較（Windows Native NTFS / `ZeroExt4` / `wsl --mount --vhd`）

#### 測定環境および条件（同一 SSD 上での公平比較）

`scripts/bench_compare_ssd.ps1` を管理者 PowerShell で実行し、全バックエンドを同一物理 NVMe SSD（`C:` ドライブ）上で計測した。`ZeroExt4` 側は `.img` ファイルの OS キャッシュ干渉を排除するため `Mount-VHD -ReadOnly` でアタッチした `\\.\PhysicalDrive4`（`win32_io.c` が実 SSD と同じ `FILE_FLAG_NO_BUFFERING` でオープン）を使用し、`wsl --mount --vhd` と同一の 1024 MB 固定サイズ VHDX（GPT Partition 1, ext4, `/model.gguf` 512 MB・`Runs: 4`）を共有した。

※ `ZeroExt4` 側はいずれも Read-Only マウントであり、**`--rw-fast-io` は不使用**（②は `--no-direct-io` による上流 `e2fsprogs` 準拠経路、③は RO 既定の Direct Extent I/O）。

| 項目 | 実環境・設定値 |
| :--- | :--- |
| 計測日時 | `2026-11-28 13:34:03 +09:00` |
| CPU / RAM | `AMD Ryzen 5 5600G with Radeon Graphics` / `63.8 GB` |
| OS / WSL2 | `Windows 10 Pro 26H2 (Build 26300)` / `WSL 2.3.26.0` (`Ubuntu`, `Kernel 5.15.167.4-microsoft-standard-WSL2`) |
| 対象 SSD | `Disk 3 [INTEL SSDPEKNU512GZ]` (`Bus=NVMe`, `Media=SSD`, `476.9 GB`, `C:`) |
| テストデータ | 決定論的非ゼロ 512 MB 連続ファイル（NTFS: `native_model.gguf`, ext4: `/model.gguf` `Runs=4`） |
| 比較 5 バックエンド | ① `Native NTFS` / ② `ZeroExt4 RO --no-direct-io` (上流 `e2fsprogs` 標準経路 / `fast-io` なし) / ③ `ZeroExt4 RO 既定` (Direct I/O ON / `fast-io` なし) / ④ `WSL2 9P (\\wsl.localhost)` / ⑤ `WSL2 Linux Kernel (Ref)`（WSL2 内直接アクセス） |
| Cold / Unbuf 条件 | Windows 側: `FILE_FLAG_NO_BUFFERING` / WSL2 側: 各測定前に `sync && echo 3 > /proc/sys/vm/drop_caches`＋Linux 直接測定は `O_DIRECT` |
| Warm / Cached 条件 | `warm` 指定により計測前に 1 MB 単位で全 512 MB を 1 周先読みしシークバック後に計測（4KB ランダムは 16,384 ops = 64 MB） |

#### 横並びサマリー（MB/s）

| Workload | Native NTFS | `ZeroExt4 Std`<br><sub>(RO `--no-direct-io` / `fast-io` なし)</sub> | `ZeroExt4 DIO`<br><sub>(RO 既定 / `fast-io` なし)</sub> | WSL2 9P | WSL2 Kernel (Ref) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `Cold/Unbuf Seq 1024KB` | `1,271.3` | `69.3` | **`1,386.2`** | `127.3` | `1,718.1` |
| `Cold/Unbuf Seq 64KB` | `1,024.4` | `67.6` | **`603.1`** | `108.0` | `451.1` |
| `Cold/Unbuf Rnd 4KB` | `28.8` | `31.3` | **`33.5`** | `9.4` | `25.6` |
| `Warm/Cached Seq 1024KB` | `10,263.5` | **`10,603.4`** | **`10,535.7`** | `192.9` | `10,544.1` |
| `Warm/Cached Seq 64KB` | `8,410.3` | `7,904.1` | **`8,471.1`** | `120.5` | `14,012.1` |
| `Warm/Cached Rnd 4KB` | `939.3` | **`984.2`** | **`1,007.9`** | `18.3` | `3,281.6` |

#### 詳細実測値（所要時間 / スループット / IOPS）

<details>
<summary>5 バックエンド全 30 項目の詳細計測ログ（クリックで展開）</summary>

1. **Windows Native (NTFS)**:
   - `UNBUF SEQ (1024KB)`: `0.403 s` / `1271.3 MB/s` (`1271 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.500 s` / `1024.4 MB/s` (`16390 IOPS`)
   - `UNBUF RND (4KB)`: `2.225 s` / `28.8 MB/s` (`7365 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.050 s` / `10263.5 MB/s` (`10264 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.061 s` / `8410.3 MB/s` (`134566 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.068 s` / `939.3 MB/s` (`240473 IOPS`)
2. **`ZeroExt4` (RO `--no-direct-io` 上流 `e2fsprogs` 標準経路 / `fast-io` なし, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `7.386 s` / `69.3 MB/s` (`69 IOPS`)
   - `UNBUF SEQ (64KB)`: `7.576 s` / `67.6 MB/s` (`1081 IOPS`)
   - `UNBUF RND (4KB)`: `2.047 s` / `31.3 MB/s` (`8002 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.048 s` / `10603.4 MB/s` (`10603 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.065 s` / `7904.1 MB/s` (`126465 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.065 s` / `984.2 MB/s` (`251949 IOPS`)
3. **`ZeroExt4` (RO 既定 Direct I/O ON / `fast-io` なし, `\\.\PhysicalDrive4` → `W:`)**:
   - `UNBUF SEQ (1024KB)`: `0.369 s` / `1386.2 MB/s` (`1386 IOPS`)
   - `UNBUF SEQ (64KB)`: `0.849 s` / `603.1 MB/s` (`9650 IOPS`)
   - `UNBUF RND (4KB)`: `1.913 s` / `33.5 MB/s` (`8566 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10535.7 MB/s` (`10536 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.060 s` / `8471.1 MB/s` (`135537 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.063 s` / `1007.9 MB/s` (`258023 IOPS`)
4. **`WSL2 9P` (Windows アプリ → `\\wsl.localhost\Ubuntu\mnt\wsl\ext4bench\model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `4.021 s` / `127.3 MB/s` (`127 IOPS`)
   - `UNBUF SEQ (64KB)`: `4.740 s` / `108.0 MB/s` (`1728 IOPS`)
   - `UNBUF RND (4KB)`: `6.786 s` / `9.4 MB/s` (`2415 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `2.654 s` / `192.9 MB/s` (`193 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `4.250 s` / `120.5 MB/s` (`1927 IOPS`)
   - `CACHED-WARM RND (4KB)`: `3.506 s` / `18.3 MB/s` (`4674 IOPS`)
5. **`WSL2 Linux Kernel Direct` (参考値: `/mnt/wsl/ext4bench/model.gguf`)**:
   - `UNBUF SEQ (1024KB)`: `0.298 s` / `1718.1 MB/s` (`1718 IOPS`)
   - `UNBUF SEQ (64KB)`: `1.135 s` / `451.1 MB/s` (`7217 IOPS`)
   - `UNBUF RND (4KB)`: `2.498 s` / `25.6 MB/s` (`6558 IOPS`)
   - `CACHED-WARM SEQ (1024KB)`: `0.049 s` / `10544.1 MB/s` (`10544 IOPS`)
   - `CACHED-WARM SEQ (64KB)`: `0.037 s` / `14012.1 MB/s` (`224194 IOPS`)
   - `CACHED-WARM RND (4KB)`: `0.020 s` / `3281.6 MB/s` (`840080 IOPS`)

</details>

---

## 3. 考察と技術的分析

### 3.1 大容量・シーケンシャル読込（64 KB〜1 MB）および `--rw` 経路の高速化要因

- **RO 既定 Direct I/O ON（`--rw-fast-io` 不使用）**: ファイルハンドル（`ReadPointerAcquire` で無競合参照）またはシャード化 `extent_cache` に保持した `ext4_extent_map_t` に対する二分探索で論理オフセットからディスク実オフセットへ即座に解決し、`fs_lock` を一切取得せず `FILE_FLAG_OVERLAPPED` 並列 `ReadFile`（`win32_io_pread_direct`）で一括読み切りを行うため、VFS 8T Seq 64KB で **`30,573.7 MB/s` (`489,180 IOPS`, `2.04 us/op`)**・1T Seq 1MB で **`7,018.4 MB/s`** に達する。
- **`--rw --rw-fast-io`（オプトイン）の最適化 vs 既定 `--rw` / `--no-direct-io`**:
  - 上流 `ext2fs_file_read` / `ext2fs_file_write` 経路（RO `--no-direct-io` および既定 `--rw`）は 4 KB ブロック単位で `ext2fs_bmap2` と I/O（さらに `fs_lock` 直列化）を反復するため、VFS 書込 Seq 1MB は `233.9 MB/s`、8T 読込 Seq 64KB は `852.5〜882.5 MB/s` にとどまる。とくに物理ディスク直読（`FILE_FLAG_NO_BUFFERING`）では毎回 4 KB 非バッファ I/O に分割されるため、実 SSD Cold Seq 1024KB は **`69.3 MB/s`** まで低下する（これを一括連続直読へバイパスする RO 既定 Direct I/O の `1,386.2 MB/s` が **20.00x** 高速な理由である）。
  - `--rw --rw-fast-io` を併用した場合は、読込側で `ctx->fs_lock` 下ではエクステント配置解決のみを行って即解放し `ctx->rwlock (Shared)` 保持のまま並列一括 `io_channel_read_blk64` を実行するよう切り替わるため、VFS 8T 読込 Seq 64KB が `882.5` → **`29,597.4 MB/s`（33.54x）**、1T 読込 Seq 1MB が `923.2` → **`7,712.1 MB/s`（8.35x）**、WinFsp 実マウントでも `UNBUF SEQ 1024KB` が `681.9` → **`2,636.5 MB/s`（3.87x）** へ劇的に改善する。
  - 書込側（`--rw --rw-fast-io`）でも `ext2fs_new_range` + `ext2fs_block_alloc_stats_range` による連続エクステントバッチ確保と一括 `io_channel_write_blk64`・整列ゼロコピー書込により、既定 `--rw` (`233.9 MB/s`) 比で `RW 1T Write Seq 1MB` **`3,714.0 MB/s`（15.88x）**・`RW 1T Overwrite 64KB` **`2,659.0 MB/s`（3.36x）** を達成する（`v0.10.1` 以降は想定外の独自実装バグによる破損リスクを避けるため、既定 `--rw` は上流 `e2fsprogs` 準拠のままとし、`--rw-fast-io` 指定時のみ有効化する安全設計としている）。

### 3.2 小ブロック（4 KB）ランダム読込における挙動とトレードオフ

1. **`FILE_FLAG_OVERLAPPED` と並列 I/O による多スレッド 4 KB 性能**:
   - Direct I/O 側の `8T Rnd 4KB` は `/model.gguf` **`1,142.5 MB/s`**・断片化 `/frag.bin` **`1,189.3 MB/s` (`304,464 IOPS`)** を安定して維持している。
   - 上流 `ext2fs_file_read` 準拠（`enable_direct_io=0`）では `/model.gguf` `445.4 MB/s`・`/frag.bin` `367.9 MB/s` となるため、RO / `--overlay` での既定 Direct I/O ON がそれぞれ **2.57x〜3.23x**（`--rw --rw-fast-io` 時は `8T Rnd 4KB` `1,942.0 MB/s`）の優位性を持つ。
2. **WinFsp 実マウント時の 4 KB 非バッファランダム性能**:
   - WinFsp 経由の `UNBUF RND (4KB)` はカーネル〜ユーザー空間ディスパッチ費用（約 `25〜28 us/op`）や NVMe 4 KB 非バッファ遅延が支配的となるため、RO `--direct-io` ON/OFF の差は僅差（実 SSD `33.5` vs `31.3 MB/s`、`.img` `148.9` vs `141.3 MB/s`）にとどまる。ただし通常のキャッシュ有効（`CACHED-WARM`）利用では NT Cache Manager により **`1,007.9 MB/s` (`258,023 IOPS`)** に達する。

### 3.3 同一 SSD 5 方式比較から得られた技術的知見・Step 1〜4 および最新分離の効果

1. **実ブロックデバイス直読（Cold / Unbuffered）で RO 既定 Direct I/O は `--no-direct-io` 比 20x・`WSL2 9P` 比 10.9x**:
   - `Cold/Unbuf Seq 1024KB`: `ZeroExt4 DIO`（RO 既定・`--rw-fast-io` 不使用）**`1,386.2 MB/s`**（`ZeroExt4 Std` `--no-direct-io` `69.3 MB/s` の **20.00x**、`WSL2 9P` `127.3 MB/s` の **10.89x**、`Native NTFS` `1,271.3 MB/s` の **1.09x**、`WSL2 Linux Kernel` `1,718.1 MB/s` の **80.7%**）。なお `v0.10.0` で一時的に標準経路側で `1,433.5 MB/s` が出ていたのはバッチ並列読込が `--no-direct-io` にも効いていたためであり、現在の `--no-direct-io` は純粋な上流 `e2fsprogs` 4 KB 分割経路（`69.3 MB/s`）となっている。
   - `Cold/Unbuf Seq 64KB`: `ZeroExt4 DIO` **`603.1 MB/s`**（`ZeroExt4 Std` `67.6 MB/s` の **8.92x**、`WSL2 9P` `108.0 MB/s` の **5.58x**、`WSL2 Linux Kernel` `451.1 MB/s` の **1.34x**）。
   - `Cold/Unbuf Rnd 4KB`: `ZeroExt4 DIO` **`33.5 MB/s` (`8,566 IOPS`)**（`Native NTFS` `28.8 MB/s` の **1.16x**、`WSL2 9P` `9.4 MB/s` の **3.56x**、`WSL2 Linux Kernel` `25.6 MB/s` の **1.31x**）。
2. **Warm / Cached で `ZeroExt4` が `Native NTFS` 同等以上・`WSL2 9P` 比 55x〜70x に到達（Step 2 の効果）**:
   - Read-Only マウント時の `FileInfoTimeout` 既定値を `(UINT32)-1` (`INFINITE`) としたことで、Windows NT Cache Manager（`CcCopyRead`）が有効化され、全 Warm 項目で Native NTFS を上回る帯域を記録した（Warm 時は OS キャッシュヒットとなるため `--no-direct-io` でも `10,603.4 MB/s` に到達するが、初回 Cold 読込は Direct I/O が 20x 高速）：
     - `Warm/Cached Seq 1024KB`: **`10,535.7 MB/s`**（`Native NTFS` `10,263.5 MB/s` の **1.03x**、`WSL2 9P` `192.9 MB/s` の **54.6x**）
     - `Warm/Cached Seq 64KB`: **`8,471.1 MB/s`**（`Native NTFS` `8,410.3 MB/s` の **1.01x**、`WSL2 9P` `120.5 MB/s` の **70.3x**）
     - `Warm/Cached Rnd 4KB`: **`1,007.9 MB/s` (`258,023 IOPS`)**（`Native NTFS` `939.3 MB/s` の **1.07x**、`WSL2 9P` `18.3 MB/s` の **55.1x**）
3. **`WSL2 9P`（`\\wsl.localhost`）経由との比較**:
   - WSL2 内の Linux カーネル直接読み取りは Warm 時に `10,544.1〜14,012.1 MB/s`（4 KB ランダム `3,281.6 MB/s`）に達するものの、Windows 側アプリから `\\wsl.localhost` 越しにアクセスする場合は 9P / HVSocket 往復が律速となり、`192.9 MB/s`（4 KB ランダムは `18.3 MB/s`）が上限となる。`ZeroExt4` は Windows ネイティブのドライブレター経由で **Cold 時最大 10.9x・Warm 時 55x〜70x** 高速にアクセスできる。

### 3.4 実運用における使い分け指針

- **既定設定（RO / `--overlay` で Direct I/O 自動有効 + `FileInfoTimeout = INFINITE`）**:
  - 初回（Cold）の大容量・実ディスク直読（`1,386.2 MB/s`）から 2 回目以降のメモリ帯域（`10.54 GB/s` / `258K IOPS`）、さらに Step 4 の Dentry キャッシュ・`FspGetDirInfoByName` による高速な Explorer / シェルメタデータ応答までそのまま最高性能を発揮する。
- **`--rw` / `--rw-fast-io` / `--no-direct-io` / `--file-info-timeout <ms>` を検討すべきケース**:
  - 直接書き込み（`--rw`）の既定は `e2fsprogs` (`ext2fs_file_read`/`write`) 準拠として安全性を最優先する。テスト環境等で高速な直接書込／読込が必要な場合のみ `--rw --rw-fast-io` を併用することで、VFS RW 書込最大 **`3,714.0 MB/s`**・8T 読込最大 **`29,597.4 MB/s`** を引き出せる（`--rw` では整合性のため `FileInfoTimeout = 1000 ms`・Direct Extent I/O 無効を維持）。
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
```RO `--no-direct-io` / RO 既定 Direct I/O ON / `--rw` 既定 / `--rw --rw-fast-io` の 4 モードを切り替えて計測）。

```powershell
# マウント側（4 モードを切り替えて計測）
.\build\zeroext4.exe mount --source $Img --mountpoint W: --no-direct-io
# または（RO 既定 Direct I/O ON）
.\build\zeroext4.exe mount --source $Img --mountpoint W:
# または（--rw 既定 / --rw --rw-fast-io）
.\build\zeroext4.exe mount --source $Img --mountpoint W: --rw
.\build\zeroext4.exe mount --source $Img --mountpoint W: --rw --rw-fast-io --no-direct-io
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
