# Perfetto トレース解析・最適化レポート

本書は、Perfetto トレース出力機能（ベースライン commit `aa37709`）を用いて取得したインプロセス VFS ベンチマークトレース（`zeroext4_bench_trace.json`）およびライブ WinFsp マウントトレース（`build/gui/zeroext4_trace.json`）を定量解析し、初期に観測された **8 スレッド時の Direct I/O 逆転現象（0.63x〜0.79x）・`ReadFile` 待機・ロック競合・Warm キャッシュ効力不足・メタデータ応答遅延** の根本原因と、Step 1〜4 最適化による改善結果をまとめたレポートである（`e2fsprogs 1.47.4` 更新後の最新総合比較は [性能評価レポート](performance-evaluation.md) を参照）。

---

## 1. 解析対象トレースとエグゼクティブサマリー

### 1.1 解析対象データ

| 項目 | VFS インプロセスベンチマークトレース | ライブ WinFsp マウントトレース |
| :--- | :--- | :--- |
| ファイル | `zeroext4_bench_trace.json`（ベースライン `aa37709` 相当） | `build/gui/zeroext4_trace.json`（比較参考: GUI 実マウント） |
| 取得コマンド / 経路 | `zeroext4_bench.exe --trace zeroext4_bench_trace.json` | `ZeroExt4Gui.exe` → `zeroext4.exe mount --trace ...` |
| サイズ / イベント数 | 約 417.5 MiB (`437,753,465 bytes`) / `2,851,378` JSON オブジェクト | 約 16.7 MiB (`17,558,468 bytes`) |
| カバー範囲の注記 | 既定のスレッド別リングバッファ（`65,536` events/thread）により先頭フェーズ（`enable_direct_io=0` 側の 1T 系と先頭 8T Seq の前半、および `enable_direct_io=1` 側の 1T Seq と 1T Rnd の前半）は折り返し。**全 6 種の 8 スレッドワークロード**（うち `M1_8T_Seq_64KB_model` は全 32,768 ops 欠損なし）および **`M1_1T_Rnd_4KB_model`** 終盤を解析対象とした。 | マウント直後から Explorer 閲覧・ファイルアクセスの一連の WinFsp コールバック（`FspGetSecurityByName`, `FspOpen`, `FspReadDirectory` 等）を記録。 |

### 1.2 エグゼクティブサマリー（最重要発見）

1. **8 スレッド Direct I/O 逆転・`ReadFile` 待機の真因は「ユーザー空間ロック競合」ではなく「カーネル `FileObject->Lock` 直列化（`FILE_FLAG_OVERLAPPED` 未指定）」**:
   - `enable_direct_io=1` 時の `1T Rnd 4KB` の `ReadFile` 平均遅延は **`3.65 µs`** だが、`8T Rnd 4KB` では **`49.11 µs`（13.5 倍悪化）**、`8T Seq 64KB` では **`79.12 µs`（最大 `37,955.5 µs` = 37.96 ms）** へ急増していた。
   - [src/io/win32_io.c](../../src/io/win32_io.c#L493-L495) の `CreateFileW` に `FILE_FLAG_OVERLAPPED` が指定されていないため、`OVERLAPPED.Offset` を渡しても Windows カーネル（`NtReadFile`）が **`FileObject->Lock` で全スレッドの `ReadFile` を完全に直列化**し、さらにスレッド間で激しい **Lock Convoy（最大 36.32 ms もの単一スレッド飢餓）** を引き起こしていた。
   - ユーザー空間の `extent_cache.lock` の待ち時間は合計 `0.4〜6.6 ms`（総時間の `0.04%〜0.13%`）にすぎず、真のボトルネックは **Windows カーネル側 I/O ハンドルの同期モード**にあった。
   - [性能評価レポート §3.2](performance-evaluation.md#32-小ブロック4-kbランダム読込における挙動とトレードオフ) で観察されていた `8T Rnd 4KB` での逆転（0.63x）も、標準経路が 8 MB LRU キャッシュヒット（74.7%〜86.3%）によって `FileObject->Lock` への突入回数を約 1/2〜1/4 に減らせていたことが大きく寄与している。
2. **実マウント Warm 性能の頭打ち原因は `FileInfoTimeout = 1000` による WinFsp キャッシュマネージャ無効化**:
   - [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c#L786) で有限値（`1000 ms`）が設定されているため Windows NT Cache Manager（`CcCopyRead`）が有効にならず、2 回目以降の読込でも毎回ユーザー空間へフォールバックしている。
3. **非同期 OVERLAPPED 化後に顕在化するユーザー空間側の 3 大ボトルネックも特定**:
   - **① `win32_io.cache_lock` のロック内 `malloc`/`free` と単一 Exclusive 競合**: 212 ms 中に 204,298 回（約 96 万回/秒）取得され、4 KB ランダム時は `wait`（`37.6〜43.7 ms`）が `hold`（`31.5〜32.4 ms`）を上回る。
   - **② 標準経路・断片化ファイルでの毎回 `ext2fs_file_open/close`＋4 KB 分割 I/O**: `/frag.bin` では `/model.gguf` 比で CPU オーバーヘッドが `1.24 µs/op`（6.7%）→ `9.89 µs/op`（38.1%）へ約 8 倍増加。
   - **③ ライブ WinFsp メタデータ経路の探索コスト**: `FspGetSecurityByName` の **37.9%（704 / 1,858 回）が `STATUS_OBJECT_NAME_NOT_FOUND`** であり、そのたびに大文字小文字無視フォールバックによる全件線形走査と `ctx->rwlock (S)` 長時間保持（最大 `3.41 ms`）を誘発している。

---

## 2. トレース定量分析結果（ベースライン: `aa37709`）

### 2.1 ベンチマークフェーズ別 スパン・`ReadFile`・ロック待ち内訳

`M0_*` は標準経路（`enable_direct_io=0`）、`M1_*` は Direct I/O 経路（`enable_direct_io=1`）を示す。`Conc` はトレース記録区間（`traced_window`）に対する延べ時間の倍率（実効並列度）。

| フェーズ（記録区間 / 記録 ops 数） | 対象 VFS スパン合計 (`Conc`) | `ReadFile` 回数・合計 (`占有率`) | `ReadFile` mean / p50 / p95 / p99 / max (µs) | ユーザー空間ロック待ち合計 (`max`) |
| :--- | ---: | ---: | ---: | :--- |
| **`M0_8T_Seq_64KB_model`**<br>(`211.94 ms` / `9,513 ops`) | `1,656.8 ms` (`7.82x`) | `42,642` 回 / `1,516.7 ms` (**91.5%**) | `35.6` / `7.6` / `130.9` / `213.4` / `529.5` | `cache_lock`: `34.9 ms` (`166.5 µs`)<br>`rwlock(S)`: `0.4 ms` (`0.4 µs`) |
| **`M0_8T_Rnd_4KB_model`**<br>(`342.19 ms` / `58,779 ops`) | `1,091.2 ms` (`3.19x`) | `27,868` 回 / `949.3 ms` (**87.0%**) | `34.1` / `4.9` / `148.9` / `725.5` / `2,885.7` | `cache_lock`: `37.6 ms` (`211.0 µs`)<br>`rwlock(S)`: `1.6 ms` (`0.9 µs`) |
| **`M0_8T_Rnd_4KB_frag`**<br>(`236.54 ms` / `39,363 ops`) | `1,021.2 ms` (`4.32x`) | `22,306` 回 / `555.8 ms` (**54.4%**) | `24.9` / `8.9` / `96.3` / `161.2` / `421.5` | `cache_lock`: `43.7 ms` (`262.0 µs`)<br>`rwlock(S)`: `1.1 ms` (`0.4 µs`) |
| **`M1_1T_Rnd_4KB_model`**<br>(`51.23 ms` / `13,061 ops`) | `49.4 ms` (`0.96x`) | `13,060` 回 / `47.7 ms` (**96.6%**) | **`3.65`** / `2.8` / `8.0` / `11.2` / `32.4` | `extent_cache`: `0.4 ms` (`0.3 µs`) |
| **`M1_8T_Seq_64KB_model`**<br>(`371.96 ms` / **`32,768 ops 全件`**) | `2,598.9 ms` (`6.99x`) | `32,768` 回 / `2,592.7 ms` (**99.8%**) | **`79.1`** / `9.8` / `26.3` / **`1,152.4`** / **`37,955.5`** | `extent_cache`: `1.1 ms` (`14.3 µs`) |
| **`M1_8T_Rnd_4KB_model`**<br>(`667.08 ms` / `104,536 ops`) | `5,157.9 ms` (`7.73x`) | `104,529` 回 / `5,133.3 ms` (**99.5%**) | **`49.1`** / `7.6` / **`213.4`** / **`371.4`** / `1,192.9` | `extent_cache`: `6.6 ms` (`127.5 µs`) |
| **`M1_8T_Rnd_4KB_frag`**<br>(`678.18 ms` / `104,534 ops`) | `5,224.9 ms` (`7.70x`) | `104,530` 回 / `5,196.2 ms` (**99.5%**) | **`49.7`** / `7.8` / **`202.7`** / **`366.3`** / **`6,190.6`** | `extent_cache`: `6.5 ms` (`1.8 µs`) |

### 2.2 `ReadFile` 遅延分布ヒストグラム（8 スレッド時）

`M1_8T_Seq_64KB` は **92.2% が 5〜20 µs** で完了しているにもかかわらず、わずか **1.1%（`>= 0.5 ms`、うち `> 2 ms` が 294 回）のロングテール** が総待機時間の大部分を支配している。一方 `M1_8T_Rnd_4KB` では `20〜500 µs` 帯が合計 **40.0%** に達している。

| ワークロード | サンプル数 | `< 5 µs` | `5–20 µs` | `20–100 µs` | `100–500 µs` | `0.5–2 ms` | `> 2 ms` (件数) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `M1_8T_Seq_64KB_model` | `32,768` | `0.0%` | `92.2%` | `6.5%` | `0.1%` | `0.2%` | **`0.9%`** (`294`) |
| `M1_8T_Rnd_4KB_model` | `104,529` | `31.7%` | `28.0%` | **`24.7%`** | **`15.3%`** | `0.3%` | `0.0%` (`0`) |
| `M0_8T_Rnd_4KB_model` | `27,868` | `52.5%` | `36.0%` | `5.4%` | `4.4%` | `1.7%` | `0.0%` (`9`) |
| `M0_8T_Rnd_4KB_frag` | `22,306` | `22.5%` | `42.2%` | `30.7%` | `4.6%` | `0.0%` | `0.0%` (`0`) |

### 2.3 標準経路（Mode 0）の `LRU Block Cache` カウンタ増分

| ワークロード | カウンタ記録区間増分 `Δhits` | 増分 `Δmisses` | ブロックヒット率 (`hits / total`) |
| :--- | ---: | ---: | ---: |
| `M0_8T_Seq_64KB_model` | `121,672` | `43,777` | `73.54%` |
| `M0_8T_Rnd_4KB_model` | `124,224` | `42,053` | `74.71%` |
| `M0_8T_Rnd_4KB_frag` | `188,923` | `29,926` | `86.33%` |

※ `/model.gguf`・`/frag.bin` のランダム 4 KB でヒット率が 74.7%〜86.3% と高いのは、8 MB LRU キャッシュへのデータブロック残存に加え、毎回の `ext2fs_file_open()` が同一の inode ブロックおよび extent ブロックを毎回参照してヒット数を押し上げていることも影響している。

### 2.4 ライブ WinFsp マウントトレース（`build/gui/zeroext4_trace.json`）

| 計測対象 | 回数 | 合計時間 | 平均 | p95 | 最大 | 補足 |
| :--- | ---: | ---: | ---: | ---: | ---: | :--- |
| `io \| ReadFile` | `2,284` | `181.09 ms` | `79.3 µs` | `168.4 µs` | `2,210.0 µs` | サイズ内訳: `4096 B` が 2,275 回（99.6%） |
| `lock \| ctx->rwlock hold (S)` | `4,245` | `137.43 ms` | `32.4 µs` | `134.7 µs` | **`3,414.2 µs`** | メタデータ探索＋inode 読込の合計保持 |
| `winfsp \| FspGetSecurityByName` | `1,858` | `114.05 ms` | `61.4 µs` | `190.3 µs` | **`2,337.7 µs`** | **`0xC0000034` (`NOT_FOUND`) が 704 回（37.9%）** |
| `winfsp \| FspReadDirectory` | `112` | `13.47 ms` | `120.3 µs` | `390.4 µs` | **`3,414.8 µs`** | 全エントリへの無条件 `read_inode_full` |
| `winfsp \| FspOpen` | `1,121` | `13.36 ms` | `11.9 µs` | `54.7 µs` | `130.6 µs` | 内部で `rwlock (S)`＋inode 読込を二重実行 |
| `lock \| win32_io.cache_lock hold (X)` | `47,342` | `12.06 ms` | `0.3 µs` | `0.7 µs` | **`904.0 µs`** | ロック内 `free`/`calloc`/`malloc` 起因のスパイク |

---

## 3. 特定されたボトルネックと最適化方針

### 3.1 Priority 1：`FILE_FLAG_OVERLAPPED` 化によるカーネル `FileObject->Lock` 直列化の解消

**トレースで観測された Lock Convoy と飢餓の実態**
- 8 スレッドが同時稼働するコア区間における `ReadFile` 完了順の時系列解析から、OS ページキャッシュ上の単純読込であるにもかかわらず **厳密な直列実行と極端なスレッド占有（Lock Convoy）** が発生していることが判明した。
  - **`M1_8T_Seq_64KB_model`（全 8 スレッド同条件 `4,096 ops/thread`、コア完了数 `18,292`）**:
    - 直前と同じ `tid` が連続して `ReadFile` を完了する確率: **`98.6%`**（公正ラウンドロビンなら ~0%、ランダムでも 12.5%）。
    - 同一 `tid` の連続独占回数: **平均 `71.73` 回** / p95 `204` 回 / p99 `273` 回 / **最大 `280` 回連続**。
    - 8 スレッド全体の平均完了間隔: **`11.33 µs/op`**（1T の `10.19 µs/op` より遅く、並列度が `1.0` 未満）。
    - 最悪飢餓例: `tid=9144` が単発の 64 KB `ReadFile` で **`36.32 ms`** ブロックされている間に、他の 7 スレッドが **`3,137` 回** の `ReadFile` を先に完了（他にも `tid=6176`: `31.62 ms` 中に他 `2,806` 回、`tid=33968`: `28.57 ms` 中に他 `2,506` 回）。
    - スレッド別所要時間: 最速 `tid=29828` が `207.5 ms` で完走する一方、最遅 `tid=9144` は `371.9 ms` まで遅延（約 1.8 倍のばらつき）。
  - **`M1_8T_Rnd_4KB_model`（コア完了数 `96,464`）**:
    - 同一 `tid` 連続完了率: **`58.9%`**（平均 `2.43` 回連続、最大 `42` 回）。
    - 8 スレッド全体の平均完了間隔: **`6.34 µs/op`**（1T の `ReadFile` 平均 `3.65 µs/op` より大幅に悪化）。
    - 最悪飢餓例: `tid=1780` が単発の 4 KB `ReadFile` で `1.19 ms` 待機する間に、他スレッドが `189` 回の `ReadFile` を完了。

**コード上の根本原因**
- [win32_io.c#L493-L495](../../src/io/win32_io.c#L493-L495) の `CreateFileW` で、`file_flags` に `FILE_FLAG_OVERLAPPED` が設定されていない（`FILE_FLAG_NO_BUFFERING` または `FILE_FLAG_RANDOM_ACCESS` のみ）。
- Windows NT I/O マネージャでは、`FILE_FLAG_OVERLAPPED` を指定せずに開いたファイルハンドル（同期ハンドル）に対して `OVERLAPPED` 構造体付きで `ReadFile` / `WriteFile` を発行しても、カーネル `NtReadFile` が **`FILE_OBJECT` 内の同期用ロック（`FileObject->Lock`）を排他取得してからディスパッチするため、同一プロセス内の全ワーカースレッドの I/O が 1 本に直列化される**。

**改善方針**
1. [win32_open](../../src/io/win32_io.c#L493-L495) で `file_flags |= FILE_FLAG_OVERLAPPED;` を付与する。
2. [raw_pread_exact / raw_pwrite_exact](../../src/io/win32_io.c#L265-L325) にて非同期完了を正しくハンドリングする:
   - 各スレッドにつき 1 個の自動／手動リセットイベント（`CreateEventW`）を `FlsAlloc` デストラクタ付きの TLS へキャッシュし、I/O ごとの `CreateEvent`/`CloseHandle` コストをゼロにする。
   - `ov.hEvent = tls_event;` を設定して `ReadFile`/`WriteFile` を呼び、`FALSE` かつ `GetLastError() == ERROR_IO_PENDING` の場合のみ `GetOverlappedResult(h, &ov, &got, TRUE)` で完了を待機する（即時完了時はイベント待機システムコールすら不要）。
3. **非同期ハンドル化の注意点**:
   - [query_handle_size_and_sector](../../src/io/win32_io.c#L75-L92) 内で実ディスクに対して呼ばれる `DeviceIoControl`（`IOCTL_DISK_GET_LENGTH_INFO`, `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`）は、ハンドルが `FILE_FLAG_OVERLAPPED` の場合 **`OVERLAPPED` 構造体の受け渡しと `ERROR_IO_PENDING` 待機が必須** となるため、合わせて対応する（`GetFileSizeEx` と `FlushFileBuffers` は `OVERLAPPED` 不要）。
4. **期待効果**:
   - `.img` 上の 8T Direct I/O 並列読込が真に並列化され、1T 対比での逆転および 38 ms 級スパイクが解消される（`8T Seq 64KB`・`8T Rnd 4KB` で数倍のスループット向上が見込まれる）。
   - 実物理ディスク（`\\.\PhysicalDriveN`、`FILE_FLAG_NO_BUFFERING`）に対しても NVMe へ **Queue Depth > 1** の複数 I/O を同時発行可能になり、実 SSD での並列読込・ランダム IOPS が向上する。

---

### 3.2 Priority 2：RO 時 `FileInfoTimeout = INFINITE` による OS キャッシュ有効化

**現状と原因**
- [性能評価レポート §2.3](performance-evaluation.md#23-同一-nvme-ssd-5-方式比較windows-native-ntfs--zeroext4--wsl---mount---vhd)（改善前計測当時）で観測した通り、実物理 SSD 上での Warm 読込性能は `Native NTFS`（`10,953.9 MB/s` / 4KB Rnd `986.2 MB/s`）に対し、`ZeroExt4 DIO` は Cold 時（`1,490.9 MB/s`）と同一水準（`1,490.5 MB/s` / 4KB Rnd `36.8 MB/s`）で頭打ちになっていた。
- 原因は [winfsp_driver.c#L786](../../src/driver/winfsp_driver.c#L786) にある `out_params->FileInfoTimeout = 1000;` である。WinFsp カーネルドライバは `FileInfoTimeout = (UINT32)-1`（`INFINITE`）のときのみ Windows NT Cache Manager（`CcCopyRead`）によるファイルデータ／メタデータキャッシュを有効化する仕様となっている。

**改善方針**
- **Read-Only マウント（`ctx->read_only == 1`）時**はバッキングの ext4 ボリュームが外部変更されない前提で `FileInfoTimeout = (UINT32)-1` を既定値とする（必要に応じて `VolumeInfoTimeout` / `DirInfoTimeout` / `SecurityTimeout` も設定可能とし、互換性確認用の無効化フラグを残す）。
- なお、OS ページキャッシュが有効化されるため、`Cc` キャッシュミス時（Cold 読込・キャッシュパージ直後）の速度を決めるのは依然として Priority 1 の物理直読パスであり、両者は補完関係にある。
- **期待効果**: RO 実マウント時の 2 回目以降のシーケンシャル・`mmap`・4 KB ランダム読込が Native NTFS 水準（約 7〜25 倍）へ到達し、Explorer 等での `FspGetSecurityByName` 再発行頻度も劇的に低下する。

---

### 3.3 Priority 3：ロック粒度縮小とロック内ヒープ確保の排除

Priority 1 で `ReadFile` 待ちが解消すると、これまで `ReadFile` の陰に隠れていたユーザー空間ロックとアトミック操作のオーバーヘッドが支配的になる。

#### (a) `win32_io.cache_lock`（[src/io/win32_io.c#L161-L240](../../src/io/win32_io.c#L161-L240)）
- **観測された問題**:
  - `M0_8T_Seq_64KB` では 212 ms 中に **204,298 回**（約 96 万回/秒）の `AcquireSRWLockExclusive` が発生。
  - `M0_8T_Rnd_4KB_model`（wait `37.6 ms` vs hold `31.5 ms` = **1.19x**）および `M0_8T_Rnd_4KB_frag`（wait `43.7 ms` vs hold `32.4 ms` = **1.35x**）では、**待ち時間が保持時間を上回る**ロック飽和状態にある。
  - 最大保持時間はベンチで `256.0 µs`、ライブ GUI トレースでは **`904.0 µs`** に達している。これは [cache_put_block](../../src/io/win32_io.c#L181-L204) が **Exclusive ロック保持中に `free()`・`calloc()`・`malloc(4096)` を毎回呼び、CRT ヒープのロック競合を内包しているため**である。
- **改善案**:
  1. **Eviction 時のメモリ構造体・バッファの In-place 再利用**: キャッシュ満杯時に `victim` を `free`→再 `malloc` せず、`victim` のハッシュリンクだけ張り替えて 4 KB バッファを上書き再利用する（ロック内ヒープ操作を完全排除）。
  2. **ハッシュ・LRU の 16〜32 シャード分割（`block & (N - 1)`）**: シャードごとに `SRWLOCK` を独立させ、8 スレッド以上での競合率を 1/16〜1/32 へ低減する。
  3. （追加余地）参照専用のヒット更新には `ref_bit`＋CLOCK 置換＋`AcquireSRWLockShared` を用い、LRU ポインタ付け替えによる Exclusive ロックを削減する。

#### (b) `extent_cache.lock` とファイルハンドルキャッシュ（[src/extent/extent_map.c#L448-L476](../../src/extent/extent_map.c#L448-L476), [src/vfs/op_read.c#L19-L35](../../src/vfs/op_read.c#L19-L35)）
- **観測された問題**:
  - トレース上の合計待ち時間は `1.1〜6.6 ms`（0.04%〜0.13%）と現時点では小さいが、`M1_8T_Rnd_4KB_model` で `max wait = 127.5 µs` が既に発生している。
  - [ext4_vfs_lower_read](../../src/vfs/op_read.c#L19-L35) は、同一ファイルの毎回の read ごとに `ext4_extent_cache_get`（`AcquireSRWLockExclusive`＋LRU 移動＋`InterlockedIncrement(&map->ref_count)`）と `ext4_extent_map_release`（`InterlockedDecrement`）を実行している。さらに [win32_aligned_read](../../src/io/win32_io.c#L359-L360) 末尾でも 2 本の `InterlockedAdd64`（`priv->stats.bytes_read`, `priv->ext2_stats.bytes_read`）が全スレッド共通のキャッシュラインを更新し続けている。
  - Priority 1 修正後に Direct I/O が 50 万〜100 万 IOPS 級へ達すると、これらの共有キャッシュラインのバウンスが次の頭打ち要因になる。
- **改善案**:
  1. [ext4_file_handle_t](../../src/vfs/ext4_vfs.h#L87-L94) に `ext4_extent_map_t *cached_extents` を保持し、`FspOpen` で 1 度だけ retain して `FspClose` で release する経路を追加（WinFsp `FspRead` パスの `extent_cache.lock` と `ref_count` 更新を **完全ゼロ化**）。
  2. `ext4_extent_cache_get` 自体は LRU 先頭要素またはヒット率安定時は **`AcquireSRWLockShared` のみで参照**し、LRU 降格閾値を超えた場合のみ Exclusive 再配置（またはシャード化）する。
  3. 統計カウンタ更新の頻度低減・スレッドローカル集約または `ext2_stats` 重複更新の見直し。

---

### 3.4 Priority 4：標準経路（`--no-direct-io` / `--rw`）の I/O 分割と冗長更新の解消

**トレースから見た定量差（`/model.gguf` vs `/frag.bin`）**
- 連続ファイル `/model.gguf`（`runs=1`）と断片化ファイル `/frag.bin`（`runs=65`, depth 1 extent tree）を `M0_8T_Rnd_4KB` で比較すると：
  - `win32_io.cache_lock` 取得回数: `2.47 回/op`（`145,414 / 58,779`）→ **`4.57 回/op`**（`179,740 / 39,363`）へ増加。
  - `ReadFile`・`cache_lock` を差し引いた **純 VFS CPU 時間**: `1.24 µs/op`（総時間の `6.7%`）→ **`9.89 µs/op`（総時間の `38.1%` / `389.3 ms`）へ約 8 倍増加**。
- **コード上の原因**:
  - [ext4_vfs_lower_read](../../src/vfs/op_read.c#L48-L60) の標準フォールバック経路は、read 1 回ごとに `ext2fs_file_open`（ヒープ確保＋inode 読込）→ `ext2fs_file_llseek` → `ext2fs_file_read`（4 KB ブロック単位ループ）→ `ext2fs_file_close` を毎回やり直している。このため `64 KB` 読込であっても常に 4 KB × 16 回の `ReadFile` に分割され、実ディスク（`FILE_FLAG_NO_BUFFERING`）でスループットが低下する直接原因になっている（[性能評価レポート §3.3](performance-evaluation.md#33-同一-ssd-5-方式比較から得られた技術的知見step-14-の効果)）。
  - また書込側 [ext4_vfs_write](../../src/vfs/op_write.c#L28-L58) も `ext2fs_file_open/close` 完了直後に、改めて `ext2fs_read_inode_full` ＋ `ext2fs_write_inode_full` を実行しており、inode 更新が重複している。

**改善方針**
1. **`--rw` モードでの安全キャッシュ付き Direct-Extent Read の解禁**: `ctx->rwlock (S)` を保持したまま、write / truncate / delete 時に即座に invalidate される extent マップを用いて `win32_aligned_read` を直接呼ぶ経路を `--rw` にも拡張する（`libext2fs` のデータブロックキャッシュと整合しない場合のみキャッシュ無効またはフラッシュ整合を保証）。これにより `--rw` および断片化ファイル読込での CPU 38% 消費と 4 KB 細切れ I/O を同時に解消できる。
2. **`ext4_vfs_write` のタイムスタンプ更新統合**: `ext2fs_file_close` の前に更新済み inode 構造体へ直接時刻を反映させ、冗長な `ext2fs_read_inode_full`/`write_inode_full` 1 往復を削減する。

---

### 3.5 Priority 5：メタデータ経路と `ctx->rwlock` 長時間保持の短縮

ライブ WinFsp マウントトレース（`build/gui/zeroext4_trace.json`）より、Explorer やシェル操作の体感速度は以下のメタデータ処理で決まっていることが判明した。

1. **存在しないファイル（負のルックアップ）の 37.9% が毎回ディレクトリ全件線形走査を誘発**:
   - `FspGetSecurityByName` 全 `1,858` 回のうち **`704` 回（`37.9%`）が `STATUS_OBJECT_NAME_NOT_FOUND` (`0xC0000034`)** を返している（Windows Explorer の `desktop.ini`, `.lnk`, `autorun.inf` 等のプローブ）。
   - [ext4_vfs_lookup_locked](../../src/vfs/op_init.c#L89-L145) は、`ctx->case_insensitive == 1` の既定設定では `ext2fs_namei` 失敗時に大文字小文字無視フォールバックのため `ext2fs_dir_iterate2` で **親ディレクトリを最初から最後まで全件線形走査（`ci_lookup_cb`）** しており、結果として `ctx->rwlock (S)` を最大 `2.34 ms` 保持し続けている。
   - **対策**: 小規模な **正／負 dentry キャッシュ**（`(parent_ino, lower_name) -> child_ino | NEGATIVE_ENOENT`、変更系 VFS 操作時に invalidate）を導入する。
2. **`FspOpen` の `ctx->rwlock` 二重取得＆inode 二重読込**:
   - [FspOpen_impl](../../src/driver/winfsp_driver.c#L232-L258) は `ext4_vfs_open` で `ctx->rwlock (S)`＋`ext2fs_read_inode` を実行した直後に、`FileInfo` 取得のため再度 `ext4_vfs_stat_ino` で `ctx->rwlock (S)`＋`ext2fs_read_inode_full` を実行している。
   - **対策**: `ext4_vfs_open` 側で `ext2fs_read_inode_full` を使い `ext4_stat_t` を同時に返す（または統合版を呼ぶ）ことでロック取得・inode 読込回数を半減する。
3. **`FspReadDirectory` の $O(N^2)$ Marker 再走査と `GetDirInfoByName` 未実装**:
   - [op_readdir.c](../../src/vfs/op_readdir.c#L36-L42) の `vfs_readdir_cb_internal` が **全ディレクトリエントリに対して無条件で `ext2fs_read_inode_full` を発行したあと**、[fsp_readdir_cb](../../src/driver/winfsp_driver.c#L687-L692) で `Marker` 到達前のエントリを捨てている。ファイル数が多いディレクトリで複数回 `FspReadDirectory` に分割されると $O(N^2)$ の inode 読込が発生し、`ctx->rwlock (S)` 最大 `3.41 ms` 保持の主因となる。
   - また [winfsp.h#L290](../../third_party/winfsp/include/winfsp/winfsp.h#L290) の `GetDirInfoByName` が未実装のため、1 件だけの検索が `ReadDirectory` 全列挙へフォールバックする場合がある。
   - **対策**: `Marker` 判定を `ext2fs_read_inode_full` より前に移動して既処理エントリの inode 読込を完全にスキップし、合わせて `GetDirInfoByName` コールバックを実装する。

---

## 4. 実施した最適化ステップと再計測手順

### 4.1 実施ステップ一覧（Step 1〜4）

| ステップ | 対象ファイル・箇所 | 変更規模 | 主な期待効果 |
| :---: | :--- | :---: | :--- |
| **Step 1** | [src/io/win32_io.c](../../src/io/win32_io.c#L493-L495)<br>(`FILE_FLAG_OVERLAPPED`＋TLS イベント完了待ち＋`DeviceIoControl` 対応) | 小〜中 | 8T `ReadFile` カーネル直列化・`38 ms` Lock Convoy を根治。`.img` 8T 並列・実 NVMe SSD QD>1 を解放。 |
| **Step 2** | [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c#L786)<br>(RO マウント時 `FileInfoTimeout = (UINT32)-1` 既定化) | 極小 | 実マウント Warm 性能を Native NTFS 水準（約 7〜25 倍）へ引き上げ＋メタデータ再照会を大幅削減。 |
| **Step 3** | [src/io/win32_io.c](../../src/io/win32_io.c#L161-L240)（LRU バッファ再利用＋シャード化）<br>[src/extent/extent_map.c](../../src/extent/extent_map.c#L448-L476)・[ext4_vfs.h](../../src/vfs/ext4_vfs.h#L87-L94)（`extent_cache` Shared 化＋ハンドル保持） | 中 | Step 1 後に顕在化するユーザー空間ロック競合・ロック内 `malloc`/`free` (`256〜904 µs` スパイク)・キャッシュラインバウンスを解消。 |
| **Step 4** | [op_init.c](../../src/vfs/op_init.c#L89-L145)・[op_readdir.c](../../src/vfs/op_readdir.c#L36-L42)・[winfsp_driver.c](../../src/driver/winfsp_driver.c#L232-L258)・[op_write.c](../../src/vfs/op_write.c#L28-L58) | 中 | 負のルックアップ高速化、`FspOpen` 二重読込解消、`FspReadDirectory` $O(N^2)$ 解消、`--rw` 経路改善。 |

### 4.2 再計測時のトレース運用メモ

- **リングバッファ容量**: 全 14 フェーズ（`enable_direct_io=0` 側 7 種＋`1` 側 7 種）を先頭から欠損なしで記録するには、1 スレッドあたりイベント数を増やして実行する：
  ```powershell
  .\build\zeroext4_bench.exe --trace zeroext4_bench_trace_after.json --trace-events 524288
  ```
- **ベンチマーク側スレッド名補足**: [tests/bench_direct_io.c](../../tests/bench_direct_io.c#L46-L48) では `threads == 1` 実行時もメインスレッドで `bench_worker` を直接呼ぶため、メインスレッド名が `"Bench-Main"` から `"Bench-Worker"` に上書きされる。改善時は `threads > 1`（別スレッド起動時）のみ `ztrace_set_thread_name("Bench-Worker")` すると Perfetto UI 上の視認性が向上する。

---

## 5. ステップ別改善効果の実測結果

> [!NOTE]
> Step 1〜4 の実装後に `zeroext4_bench.exe --trace ... --trace-events 524288` 等で再計測した Before / After の実績値を以下にまとめる（`e2fsprogs 1.47.4` 反映後の総合評価は [性能評価レポート](performance-evaluation.md) を参照）。

| 指標 | Before (`aa37709`) | Step 1 後 (`OVERLAPPED`) | Step 2 後 (`INFINITE` Timeout) | Step 3 後 (`Cache Shard`) | Step 4 後 (`Metadata / Write Opt`) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `VFS DIO 8T Seq 64KB` (`MB/s` / `ReadFile` max) | `5,404.1 MB/s` / `37.96 ms` (`98.6%` 連続独占) | **`31,835.1 MB/s`** / `0.240 ms` (`0.4%` 連続) | — | **`32,795.1 MB/s`** (`524,721 IOPS`) | **`33,066.3 MB/s`** <br><sub>(3 回中央値; 最大 `33,225.9 MB/s` / 回帰なし)</sub> |
| `VFS DIO 8T Rnd 4KB /model.gguf` (`MB/s` / `ReadFile` mean) | `632.4 MB/s` (`0.63x`) / `49.11 µs` | **`1,155.4 MB/s`** (`0.88x`) / `24.51 µs` | — | **`1,248.8 MB/s`** (`24.19 µs`) | **`1,237.7 MB/s`** <br><sub>(3 回中央値; `316,860 IOPS` / 回帰なし)</sub> |
| `VFS DIO 8T Rnd 4KB /frag.bin` (`MB/s` / `ReadFile` mean) | `640.6 MB/s` (`0.79x`) / `49.71 µs` | **`1,260.7 MB/s`** (`1.13x`) / `24.46 µs` | — | **`1,241.4 MB/s`** (`24.01 µs`) | **`1,245.3 MB/s`** <br><sub>(3 回中央値; `318,798 IOPS` / 回帰なし)</sub> |
| `同一 SSD Warm/Cached Seq 1024KB` (`MB/s`) | `1,490.5 MB/s` | _未計測_ | `9,190.0 MB/s` <br><sub>(`.img` ライブ A/B: `2,359.0` → **`9,190.0 MB/s`**; 同一 SSD スクリプト未実施)</sub> | — | — |
| `同一 SSD Warm/Cached Rnd 4KB` (`MB/s`) | `36.8 MB/s` | _未計測_ | `1,015.3 MB/s` <br><sub>(`.img` ライブ A/B: `134.1` → **`1,015.3 MB/s`**; 同一 SSD スクリプト未実施)</sub> | — | — |
| `cache_lock wait` 合計 / max (`M0_8T_Rnd_4KB_frag`) | `43.7 ms` / `262.0 µs` | `798.0 ms` / `353.5 µs` | — | **`44.3 ms`** (`X:16.8 + S:27.5`) / `221.2 µs` | — |
| `ライブ RO 負ルックアップ 2000 回` (`FspGetSecurityByName`) | — | — | — | `220.72 ms` (`59.11 µs/call`) | **`52.98 ms`** (`4.94 µs/call`, **4.17x / コールバック 11.97x**) |
| `ライブ RO 単一 FindFirst 1000 回` (`FspGetDirInfoByName`) | — | — | — | `344.41 ms` (`FspReadDirectory` 1450 回) | **`122.98 ms`** (`FspGetDirInfoByName` 1000 回 / **`2.34 µs`**) |
| `ライブ RO 200ファイル列挙 x50` (`ctx->rwlock (S)` 合計) | — | — | — | `68.08 ms` (`550.60 ms` hold) | **`32.12 ms`** (`45.41 ms` hold, **-91.8%**) |

### 5.1 Step 1 (`FILE_FLAG_OVERLAPPED`) 効果検証結果

1. **無計測ベンチマーク（`zeroext4_bench.exe`）の変化**:
   - **Direct I/O (`enable_direct_io=1`)**:
     - `8T Seq 64KB /model.gguf (2048MB)`: `5,404.1 MB/s` (`0.379 s`) → **`31,835.1 MB/s` (`0.064 s` / `509,361 IOPS`)**（**5.89x**、約 31.8 GB/s）
     - `8T Rnd 4KB /model.gguf (131K ops)`: `632.4 MB/s` → **`1,155.4 MB/s` (`295,791 IOPS`)**（**1.83x**）
     - `8T Rnd 4KB /frag.bin (131K ops)`: `640.6 MB/s` → **`1,260.7 MB/s` (`322,751 IOPS`)**（**1.97x**、Mode 0 の `1,117.6 MB/s` を再逆転し **1.13x** へ）
   - **標準経路 (`enable_direct_io=0`)**:
     - `8T Seq 64KB /model.gguf (2048MB)`: `2,834.1 MB/s` → **`4,968.9 MB/s`**（**1.75x**）
     - `8T Rnd 4KB /model.gguf`: `1,000.4 MB/s` → **`1,318.5 MB/s`**（**1.32x**）
     - `8T Rnd 4KB /frag.bin`: `812.1 MB/s` → **`1,117.6 MB/s`**（**1.38x**）
2. **Perfetto トレース解析（`--trace-events 524288`）の変化**:
   - **`FileObject->Lock` 直列化・Lock Convoy の完全解消**:
     - `M1_8T_Seq_64KB`: 同一スレッド連続完了率が `98.6%`（最大 280 回連続）→ **`0.4%`（最大 4 回）** へ低下。`ReadFile` mean `79.12` → `14.74 µs`（p99 `1,152.4` → `60.4 µs`、max `37,955.5` → `239.6 µs` で最大遅延を **158 分の 1** に短縮）。
     - `M1_8T_Rnd_4KB`: 同一スレッド連続完了率が `58.9%` → `4.2%`（`/frag.bin` `4.1%`）、`ReadFile` mean `49.11` → `24.51 µs`（max `1,192.9` → `425.2 µs`）。
   - **次のボトルネック（Step 3 対象の顕在化）**:
     - `ReadFile` 待機が解けた結果、Mode 0 では **`win32_io.cache_lock` の競合が一気に顕在化**（`M0_8T_Seq_64KB`: `wait 1,337.9 ms` / max `380.6 µs`、`M0_8T_Rnd_4KB_model`: `wait 889.7 ms` vs `hold 122.8 ms` [wait/hold **7.25x**]、`M0_8T_Rnd_4KB_frag`: `wait 798.0 ms`）。
     - Mode 1 では `extent_cache.lock` の待ちが `M1_8T_Seq_64KB` で `7.3 ms`（`63.5 ms` 中、max `70.3 µs`）、`M1_8T_Rnd_4KB` で `14.7〜16.5 ms`（max `109.7〜249.1 µs`）発生している。

### 5.2 Step 2 (RO 時 `FileInfoTimeout = INFINITE`) 効果検証結果

128 MB の作業用 ext4 イメージ（64 MB 非ゼロ `/model.gguf`）を実 WinFsp ドライブ（`Z:`、RO 既定 Direct I/O + `--trace` 有効）としてマウントし、`--file-info-timeout 1000`（従来相当）と既定 `INFINITE`（Step 2）で同一測定（`zeroext4_bench.exe --read` 4 種連続）を比較した。

1. **実 WinFsp マウント A/B ベンチマーク比較（64 MB `/model.gguf`）**:
   | ワークロード | Timeout = `1000 ms` | Timeout = `INFINITE` (Step 2 既定) | 速度比 |
   | :--- | ---: | ---: | ---: |
   | `CACHED-WARM SEQ (1024KB)` | `2,359.0 MB/s` (`2,359 IOPS`) | **`9,190.0 MB/s`** (`9,190 IOPS`) | **3.90x** |
   | `CACHED-WARM SEQ (64KB)` | `1,666.7 MB/s` (`26,667 IOPS`) | **`8,027.9 MB/s`** (`128,446 IOPS`) | **4.82x** |
   | `CACHED-WARM RND (4KB)` (16K ops) | `134.1 MB/s` (`34,328 IOPS`) | **`1,015.3 MB/s`** (`259,927 IOPS`) | **7.57x** |
   | `UNBUF SEQ (1024KB)` | `2,607.0 MB/s` (`2,607 IOPS`) | `2,704.6 MB/s` (`2,705 IOPS`) | `1.04x` |

2. **Perfetto トレース解析によるコールバック回数比較**:
   - **`FspRead` 呼び出し回数・ユーザー空間転送量**:
     - `1000 ms`: **`17,736` 回** / `448.1 MB` / 合計 `243.03 ms`（ウォーム時も全読込がユーザー空間へ到達）
     - `INFINITE`: **`131` 回** / `128.0 MB` / 合計 `43.86 ms`（**`FspRead` 回数を 99.3% 削減**。初回 64 MB ウォームアップ＋明示 `UNBUF` 64 MB の 128 MB のみがユーザー空間を通過し、3 種の `CACHED-WARM` 計測中はカーネル `CcCopyRead` で完結）
   - **メタデータ照会回数**: `FspGetSecurityByName` が `10` → `8` 回、`FspOpen` が `10` → `8` 回へ減少。
   - **CLI オーバーライド**: 外部変更を即座に追従したい用途向けに `zeroext4 mount --file-info-timeout <ms|infinite>` を追加した（RW および書き込み可能 Overlay は引き続き既定 `1000 ms` を維持）。

### 5.3 Step 3 (キャッシュシャーディング・Shared ロック化・ハンドルキャッシュ) 効果検証結果

1. **無計測ベンチマーク（`zeroext4_bench.exe`）の変化（Step 1 比）**:
   - **標準経路 (`enable_direct_io=0`)**: `win32_io.cache_lock` の 32 シャード＋Shared 参照＋インプレース eviction により大きく伸長：
     - `8T Seq 64KB /model.gguf (2048MB)`: `4,968.9 MB/s` (`0.412 s`) → **`5,715.4 MB/s`** (`0.358 s`, **+15.0%**、Baseline 比 **2.02x**)
     - `8T Rnd 4KB /model.gguf (131K ops)`: `1,318.5 MB/s` → **`1,651.7 MB/s`** (`422,833 IOPS`, **+25.3%**、Baseline 比 **1.65x**)
     - `8T Rnd 4KB /frag.bin (131K ops)`: `1,117.6 MB/s` → **`1,343.2 MB/s`** (`343,854 IOPS`, **+20.2%**、Baseline 比 **1.65x**)
   - **Direct I/O (`enable_direct_io=1`)**:
     - `8T Seq 64KB /model.gguf (2048MB)`: `31,835.1 MB/s` → **`32,795.1 MB/s`** (`0.062 s` / **`524,721 IOPS`**)
     - `8T Rnd 4KB /model.gguf (131K ops)`: `1,155.4 MB/s` → **`1,248.8 MB/s`** (`319,702 IOPS`, **+8.1%**)
     - `8T Rnd 4KB /frag.bin (131K ops)`: **`1,241.4 MB/s`** (`317,808 IOPS`)
2. **Perfetto トレース解析（`--trace-events 524288`）によるロック待ち変化（Step 1 比）**:
   - **`win32_io.cache_lock` 競合の劇的縮小（Mode 0）**:
     - `M0_8T_Seq_64KB`: 合計ロック待ち `1,337.9 ms` (`Conc=2.90`) → **`328.0 ms`** (`wait(X)=227.5 ms`, `wait(S)=100.5 ms`, `Conc=0.83`, **4.08x 削減**)
     - `M0_8T_Rnd_4KB_model`: 合計ロック待ち `889.7 ms` (`Conc=2.05`) → **`33.0 ms`** (`wait(X)=18.8 ms`, `wait(S)=14.2 ms`, `Conc=0.11`, **27.0x 削減 / -96.3%**)
     - `M0_8T_Rnd_4KB_frag`: 合計ロック待ち `798.0 ms` (`Conc=1.56`) → **`44.3 ms`** (`wait(X)=16.8 ms`, `wait(S)=27.5 ms`, `Conc=0.11`, **18.0x 削減 / -94.4%**)
   - **`extent_cache.lock` の最大スパイク縮小（Mode 1、インプロセス計測）**:
     - `M1_8T_Seq_64KB`: wait 合計 `7.3 ms` → **`2.9 ms`**（max `70.3 µs` → **`14.3 µs`**）
     - `M1_8T_Rnd_4KB_model` / `frag`: max wait `109.7 µs` / `249.1 µs` → **`13.2 µs` / `9.3 µs`**（最大 26.8x 短縮、Shared 保持平均 **`0.04 µs`**）
     - あわせて [FspRead_impl](../../src/driver/winfsp_driver.c#L366) → `ext4_vfs_read_handle` により、ライブ WinFsp マウント経由の同一ファイル読込時は `ext4_file_handle_t` に保持した `cached_extents` を参照するため、`extent_cache.lock` の取得自体を初回のみへ削減した。
   - **次の残存ボトルネック（Step 4 対象）**:
     - `M0_8T_Rnd_4KB_frag` の総 VFS 時間 `2,958.3 ms` から `ReadFile` (`1,397.6 ms`) と `cache_lock` (`139.8 ms`) を引いた約 **`1,421 ms`（10.8 µs/op、約 48%）** は、毎回の `ext2fs_file_open/llseek/read/close` による純粋な `libext2fs` CPU コストが占めている。またライブ GUI トレース側のメタデータ経路（負のルックアップ、`FspOpen` 二重読込、`FspReadDirectory`）が Step 4 の改善対象となる。

### 5.4 Step 4 (メタデータ経路・書込最適化) 効果検証結果

200 ファイルを含む 32 MB の ext4 イメージ（`/bigdir/file_0000.dat`〜`file_0199.dat` 各 4 KB）を実 WinFsp ドライブとしてマウントし、カーネル側のメタデータキャッシュを無効化した `--file-info-timeout 0`（＋`--trace-events 524288`）条件でユーザー空間コールバックの純粋な改善率を Before (`cd5e279`) / After（Step 4 実装＋`PassQueryDirectoryFileName = 1` 有効化）で A/B 比較した。

1. **実装した最適化項目**:
   - **16 シャード正／負 Dentry キャッシュ**（[src/vfs/op_init.c](../../src/vfs/op_init.c)）: 16 シャード×256 スロット（計 4,096 エントリ）、Shared ロック参照・CLOCK インプレース置換・世代エポックによる $O(1)$ 一括無効化（`op_create`, `op_mkdir`, `op_unlink`, `op_rename`, `op_symlink` で `ctx->rwlock (X)` 保持下に無効化）。大文字小文字無視時にもディスク上の実ケースパスを保持し、`ext4_vfs_lookup_locked_ex` で `CasePreservedNames` を維持。
   - **単一ロック・単一 inode 読込の統合 Open+Stat / Lookup+Stat**（[op_open.c](../../src/vfs/op_open.c), [op_getattr.c](../../src/vfs/op_getattr.c), [winfsp_driver.c](../../src/driver/winfsp_driver.c)）: `ext4_vfs_open_stat` により `FspOpen` / `FspCreate` 直後の 2 回目の `ext4_vfs_stat_ino` を廃止。
   - **早期 Marker スキップ＋`FspGetDirInfoByName` 実装**（[op_readdir.c](../../src/vfs/op_readdir.c), [winfsp_driver.c](../../src/driver/winfsp_driver.c)）: `ext4_vfs_readdir_from` で `ext2fs_read_inode_full` より前に Marker 照合を行い既処理エントリの inode 読込を回避。さらに `FspGetDirInfoByName` を実装し、`winfsp_driver_build_volume_params` で `PassQueryDirectoryFileName = 1` を設定して WinFsp が単一ファイル検索を `FspReadDirectory` にフォールバックしないようにした。
   - **書込時冗長 inode I/O 削減**（[op_write.c](../../src/vfs/op_write.c), [winfsp_driver.c](../../src/driver/winfsp_driver.c)）: `ext4_vfs_write_stat` により 1 回の `ctx->rwlock (X)`＋1 回の `ext2fs_read_inode_full`＋`ext2fs_file_open2` で 256B 拡張時刻と `FileInfo` を同時に返し、`FspWrite` 後の `ext4_vfs_stat_ino` と二重 inode 読込を削減。
   - **付随バグ修正（[closefs.c](../../third_party/e2fsprogs/lib/ext2fs/closefs.c)）**: 無計測 160 MB `zeroext4_bench.exe` 検証時に `write_backup_super_and_bgd` が 4 KB ブロック環境でも `sizeof(*sb_copy)` (1,024 B) しか確保せず `io_channel_write_blk64` に渡していたため、ヒープページ境界で `win32_aligned_write` が `0xC0000005`（読み出し範囲外）を起こし得る既存バグを発見・修正し、140 MB マルチグループ回帰テスト（`test_multi_group_backup_superblock_flush`）を追加した。

2. **ライブ WinFsp ワークロード計測比較（`--file-info-timeout 0`）**:
   | ワークロード（Win32 API） | Before (`cd5e279`) | Step 4 後 (`PassQueryDirectoryFileName=1`) | 速度比 |
   | :--- | ---: | ---: | ---: |
   | `RO_NegLookup_2000` (存在しない 5 種×400 `GetFileAttributesW`) | `220.72 ms` (`110.36 µs/op`) | **`52.98 ms`** (`26.49 µs/op`, `37,749 ops/s`) | **4.17x** |
   | `RO_PosOpenClose_1000` (200 ファイル大文字パス×5 `CreateFileW`) | `350.70 ms` (`350.70 µs/op`) | **`233.79 ms`** (`233.79 µs/op`, `4,277 ops/s`) | **1.50x** |
   | `RO_SingleFindFirst_1000` (単一ファイル正500+負500 `FindFirstFileW`) | `344.41 ms` (`344.41 µs/op`) | **`122.98 ms`** (`122.98 µs/op`, `8,131 ops/s`) | **2.80x** |
   | `RO_ReadDir200_50x` (200 ファイル全列挙 `FindFirst/NextFileW` ×50) | `68.08 ms` (`1.362 ms/round`) | **`32.12 ms`** (`0.642 ms/round`, `entries=10100`) | **2.12x** |
   | `RW_Write_1000x4KB` (`FILE_FLAG_WRITE_THROUGH` 4KB 追記500+上書500) | `46.84 ms` (`0.047 ms/op`) | **`46.09 ms`** (`0.046 ms/op`, `21,696 ops/s`) | `1.02x` |

3. **Perfetto トレース解析（Before vs After コールバック別内訳）**:
   - **RO メタデータトレース（総イベント数 `817,185` → `101,093`、-87.6%）**:
     | イベント | Before 回数 / 合計 (`平均`) | Step 4 後 回数 / 合計 (`平均`) | 改善効果 |
     | :--- | ---: | ---: | ---: |
     | `winfsp\|FspGetSecurityByName` | `4,052` / `239.532 ms` (`59.11 µs`) | `4,052` / **`20.005 ms`** (**`4.94 µs`**) | **11.97x 高速化** (`-91.6%`) |
     | `winfsp\|FspOpen` | `2,052` / `78.828 ms` (`38.42 µs`) | `2,052` / **`2.854 ms`** (**`1.39 µs`**) | **27.62x 高速化** (`-96.4%`) |
     | `winfsp\|FspReadDirectory` | `1,450` / `227.418 ms` (`156.84 µs`) | **`450`** / **`15.668 ms`** (**`34.82 µs`**) | **14.51x 削減** (`-93.1%`) |
     | `winfsp\|FspGetDirInfoByName` | `0`（`ReadDirectory` へフォールバック） | **`1,000`** / **`2.345 ms`** (**`2.34 µs`**) | **単一検索直行を達成** |
     | `lock\|ctx->rwlock hold (S)` | `15,712` / `550.595 ms` (`35.04 µs`) | `13,660` / **`45.408 ms`** (**`3.32 µs`**) | **12.13x 短縮** (`-91.8%`) |
     | `lock\|win32_io.cache_lock hold (S)` | `384,084` / `25.258 ms` | **`28,090`** / **`2.917 ms`** | **メタデータ読込 -92.7%** |
   - **RW 4KB 書込トレース（`1,000` 回 `FspWrite`）**:
     - `lock|ctx->rwlock hold (S)` が `1,023` 回（`1.017 ms`）→ **`20` 回**（`0.220 ms`, **-98.0%**）へ激減し、`win32_io.cache_lock hold (S)` も `7,097` → **`5,082` 回**（`-28.4%`）に減少した（壁時計時間の約 65.6% [`18.967 ms` / `28.897 ms`] は同期 `WriteFile` ディスク I/O が支配）。

4. **無計測 `zeroext4_bench.exe` 回帰チェック（3 回連続実行）**:
   - 全 3 回とも `EXIT=0` で完了。Mode 1 (`enable_direct_io=1`) の 3 回中央値は `8T Seq 64KB` **`33,066.3 MB/s`**（最大 `33,225.9 MB/s` / `531,615 IOPS`）、`8T Rnd 4KB /model.gguf` **`1,237.7 MB/s`**、`/frag.bin` **`1,245.3 MB/s`**、Mode 0 `8T Rnd 4KB /model.gguf` **`1,660.8 MB/s`**（最大 `1,725.9 MB/s`）・`/frag.bin` **`1,368.9 MB/s`** となり、Step 3 比で性能回帰がないことを確認した。
