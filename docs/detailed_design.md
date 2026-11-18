# 詳細設計書 (`libext2fs-win` / `ext4fsp`)

本書は、`libext2fs-win` のディレクトリ構造、CMake ビルド定義、互換レイヤー、`win32_io`、ディスク・ジャーナル検査、VFS (`op_*`)、エラー変換、および WinFsp ドライバインターフェースの詳細仕様を記述する。

---

## 1. ディレクトリ構成とビルドターゲット

```text
d:\libext2fs-win\
├── CMakeLists.txt
├── LICENSE                               # GPLv2
├── README.md
├── docs/
│   ├── requirements.md                   # 要件定義書
│   ├── architecture.md                   # アーキテクチャ設計書・ADR
│   ├── detailed_design.md                # 本詳細設計書
│   └── testing_and_usage.md              # テスト報告書・利用ガイド
├── scripts/
│   ├── generate_e2fsprogs_headers.py     # ext2_err.et.in / ext2_types.h.in -> generated headers
│   ├── update_e2fsprogs.ps1              # e2fsprogs 更新＆ビルド検証自動化スクリプト
│   └── build_and_test.ps1                # MSVC + Ninja + CTest 実行スクリプト
├── compat/
│   ├── msvc_compat.h                     # /FI 強制インクルード用互換シム
│   └── include/                          # POSIX/Autotools 代替ヘッダ
├── third_party/
│   ├── e2fsprogs/                        # 上流 lib/et, lib/ext2fs（無改変分離）
│   └── winfsp/include/winfsp/winfsp.h    # WinFsp ABI 定義 & 動的ローダー
├── src/
│   ├── common/                           # ext4_errno.h / .c
│   ├── io/                               # win32_io.h / .c
│   ├── disk/                             # partition_table, superblock_probe, disk_scanner
│   ├── extent/                           # extent_map.h / .c (ダイレクト・エクステント直結I/O)
│   ├── overlay/                          # overlay_vfs.h / .c (NTFS Upperdir OverlayFS)
│   ├── vfs/                              # ext4_vfs.h, op_*.c
│   ├── driver/                           # winfsp_driver.h / .c
│   └── cli/                              # main.c (ext4fsp.exe)
└── tests/                                # test_main.c + 単体・結合テスト群
```

### CMake ターゲット構成
1. **`generate_e2fsprogs_headers`（カスタムターゲット＋configure 時生成）**:
   - Python 3 実行ファイルを `%LOCALAPPDATA%\Python\bin\python.exe` 優先で検出し、`scripts/generate_e2fsprogs_headers.py` を実行して `${CMAKE_BINARY_DIR}/generated/ext2fs/ext2_err.h`, `${CMAKE_BINARY_DIR}/generated/ext2_err.c`, `${CMAKE_BINARY_DIR}/generated/ext2fs/ext2_types.h` を生成する。
2. **`ext2fs`（静的ライブラリ）**:
   - ソース: `third_party/e2fsprogs/lib/et/com_err.c`, 生成された `ext2_err.c`, `third_party/e2fsprogs/lib/ext2fs/*.c`（OS 依存の `unix_io.c` 等は除外）
   - コンパイルオプション (MSVC): `/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_compat.h` `/W3`
   - デバッグ形式 (`CMP0141`): `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT = "Embedded"`（`/Z7` により `mspdbsrv.exe` 非依存でビルド）
3. **`ext4fsp_core`（静的ライブラリ）**:
   - ソース: `src/common/*.c`, `src/io/*.c`, `src/disk/*.c`, `src/extent/*.c`, `src/overlay/*.c`, `src/vfs/*.c`, `src/driver/*.c`
   - リンク: `ext2fs`, `advapi32`, `shell32`, `user32`
4. **`ext4fsp`（実行ファイル）**:
   - ソース: `src/cli/main.c`
   - リンク: `ext4fsp_core`, `ext2fs`, `dbghelp`
5. **`ext4fsp_tests`（実行ファイル / CTest 登録）**:
   - ソース: `tests/*.c`
   - CTest スイート: `win32_io`, `partition_and_probe`, `vfs_operations`, `winfsp_callbacks`, `extent_and_overlay`

---

## 2. `e2fsprogs` 互換レイヤー詳細 (`compat/` & `scripts/`)

### 2.1 `compat/msvc_compat.h`
`CMakeLists.txt` の `ext2fs` ターゲットにのみ `/FI` で強制インクルードする。
- `_CRT_SECURE_NO_WARNINGS`, `_CRT_NONSTDC_NO_DEPRECATE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX` を定義。
- `_MSC_VER` 環境向けに `__attribute__(x)` 無効化、`__inline__` → `__inline`、`SSIZE_T` による `ssize_t` 定義、`strcasecmp` → `_stricmp` / `strncasecmp` → `_strnicmp`、および上流コード由来の変換/パディング警告抑制 `#pragma warning(disable: ...)` を記述。

### 2.2 `compat/include/`
- `config.h`: `PACKAGE "e2fsprogs"`, `VERSION "1.47.2"`, `HAVE_STDINT_H 1`, `HAVE_INTTYPES_H 1`, `HAVE_STDLIB_H 1`, `HAVE_STRING_H 1`, `HAVE_ERRNO_H 1`, `HAVE_SYS_TYPES_H 1`, `SIZEOF_LONG 4`, `SIZEOF_LONG_LONG 8`, `SIZEOF_OFF_T 8`, `HAVE_EXT2_IOCTLS 0` 等を定義。
- `unistd.h`, `strings.h`, `utime.h`, `sys/time.h`, `sys/ioctl.h`, `sys/resource.h`: POSIX ヘッダの最小互換スタブを提供。

### 2.3 `scripts/generate_e2fsprogs_headers.py`
- `third_party/e2fsprogs/lib/ext2fs/ext2_err.et.in` から `error_table` 名と 1 行または複数行形式の `ec <SYMBOL>, "<MSG>"` を抽出。
- `com_err` 標準エンコーディング（`ext2` テーブルの場合 `EXT2_ET_BASE = 2133571328L`）でエラー定数ヘッダ `ext2fs/ext2_err.h` およびエラーテーブル定義 `ext2_err.c` を生成。
- `ext2_types.h.in` から `@SIZEOF_*@` 置換済みの `ext2fs/ext2_types.h` を生成。

---

## 3. Windows ネイティブ I/O 層詳細 (`src/io/win32_io.h`, `win32_io.c`)

### 3.1 構成構造体と API
```c
typedef struct win32_io_config {
    uint64_t partition_offset_bytes; /* パーティション開始オフセット (bytes) */
    uint64_t partition_size_bytes;   /* パーティションサイズ (0 = 自動検出) */
    uint32_t sector_size;            /* 物理セクタサイズ (0 = 既定4096/デバイス検出) */
    uint32_t max_cache_blocks;       /* LRU キャッシュ上限ブロック数 (0 = 既定1024) */
    int      read_only;              /* 1 = GENERIC_READ のみ・書込全拒否 */
    int      disable_cache;          /* 1 = LRU キャッシュ無効 */
} win32_io_config_t;

typedef struct win32_io_stats {
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t rmw_operations;
    uint64_t device_size_bytes;
    uint32_t effective_sector_size;
} win32_io_stats_t;

extern struct struct_io_manager struct_win32_io_manager;
#define win32_io_manager (&struct_win32_io_manager)

void win32_io_set_next_config(const win32_io_config_t *cfg);
void win32_io_clear_next_config(void);
void win32_io_get_stats(io_channel channel, win32_io_stats_t *out_stats);
uint64_t win32_io_get_device_size(io_channel channel);
errcode_t win32_io_pread_direct(io_channel channel, uint64_t part_rel_offset,
                                void *dst, size_t len);
```

### 3.2 動作仕様
1. **ハンドルオープン**: `ext4_utf8_to_wide` で UTF-16 変換後、`CreateFileW` を実行。
   - `read_only != 0`: `GENERIC_READ`, `FILE_SHARE_READ | FILE_SHARE_WRITE`
   - `read_only == 0`: `GENERIC_READ | GENERIC_WRITE`, `FILE_SHARE_READ | FILE_SHARE_WRITE`
   - 物理デバイス（`\\.\` 始まり）では `FILE_FLAG_NO_BUFFERING`、イメージファイルでは `FILE_FLAG_RANDOM_ACCESS` を付与し、`IOCTL_DISK_GET_LENGTH_INFO` / `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`（または `GetFileSizeEx`）でデバイスサイズ・セクタサイズを取得する。
   - マルチスレッドでのマウント安全性を保つため、`win32_io_set_next_config` は `__declspec(thread)` スレッドローカル変数へ設定を保持する。
2. **パーティション境界ガード**:
   - 論理オフセット `logical_off = block * channel->block_size` に対し、`logical_off > partition_size || len > partition_size - logical_off` の場合は即座に `EXT2_ET_LLSEEK_FAILED` を返す。
   - 物理オフセットは `phys_off = partition_offset + logical_off`。
3. **セクタ境界・メモリアライメント ＆ Read-Modify-Write (RMW)**:
   - `(phys_off % sector_size) == 0 && (len % sector_size) == 0 && ((uintptr_t)buf % sector_size) == 0` の場合のみゼロコピー直接 `OVERLAPPED` Read/Write を実行。
   - それ以外は `_aligned_malloc(..., sector_size)` バッファを用いて `[aligned_start, aligned_end)` 単位で Read-Modify-Write を行う（書き込み RMW 発生時は `rmw_operations` をインクリメント）。
4. **スレッドセーフ LRU ブロックキャッシュ**:
   - `SRWLOCK cache_lock` で保護されたハッシュ表（2048 バケット）＋双方向リストによる LRU キャッシュ。
   - 単一ブロック Read 時（`count == 1`）にキャッシュ参照/格納を行い、Write 時は Write-Through で対象ブロック更新または範囲無効化を実施する。`set_blksize` 変更時はキャッシュを自動クリアする。

---

## 4. ディスク・パーティション・スーパーブロック検査詳細 (`src/disk/`)

### 4.1 `partition_table.h` / `.c`
- `pt_read_partitions(const char *path_utf8, uint32_t sector_size_hint, pt_table_result_t *out)`
- `pt_resolve_partition(const char *path_utf8, uint32_t partition_number, uint64_t *out_offset, uint64_t *out_size, uint32_t *out_sector_size)`
- 解析手順:
  1. **GPT**: 保護 MBR (`0xEE`) 確認後、`sector_size_hint`（未指定時 512）で `"EFI PART"` ヘッダを探索。未検出かつ保護 MBR が存在する場合は `4096` で再試行し、Type GUID・LBA 範囲・UTF-16LE パーティション名を抽出。
  2. **MBR + EBR**: `0x55AA` 確認後、基本パーティション 1〜4 を解析。拡張パーティション (`0x05`, `0x0F`, `0x85`) 検出時は訪問済み LBA チェックで無限ループを防ぎつつ EBR チェーンをたどり Partition 5 以降を列挙。
  3. **RAW**: 先頭が直接ファイルシステムスーパーブロックである場合は `style = "RAW"`, `partition_number = 0` として扱う。

### 4.2 `superblock_probe.h` / `.c`
- `ext4_probe_superblock_buffer(const uint8_t *sb_1024, ext4_probe_result_t *out)` および `ext4_probe_partition(...)`
- 検査項目:
  - Magic `0xEF53`、FS 種別判定（`EXT4` / `EXT3` / `EXT2`）、UUID、ボリュームラベル、Feature フラグ。
  - JBD2 ジャーナル検査（`s_journal_inum` のブロック 0 を `ext2fs_bmap2` 経由で読み出し、Big-Endian `JBD2_MAGIC_NUMBER = 0xC03B3998` ヘッダの `s_start` at `+0x1C` および `s_errno` at `+0x20` を解析）：
    - `EXT4_JOURNAL_NONE`: `!HAS_JOURNAL`
    - `EXT4_JOURNAL_CLEAN`: `!INCOMPAT_RECOVER`, `!EXT2_ERROR_FS`, `s_start == 0`, `s_errno == 0`
    - `EXT4_JOURNAL_NEEDS_RECOVERY`: `EXT3_FEATURE_INCOMPAT_RECOVER` または `s_start != 0`
    - `EXT4_JOURNAL_HAS_ERRORS`: `s_state & EXT2_ERROR_FS` または `s_errno != 0`

---

## 5. VFS コア層詳細 (`src/vfs/`)

### 5.1 コンテキストとロック順序規則
```c
typedef struct ext4_vfs_ctx {
    ext2_filsys               fs;
    SRWLOCK                   rwlock;
    int                       read_only;
    int                       lower_read_only;
    int                       dirty_detected;
    int                       fell_back_to_ro;
    int                       case_insensitive;
    int                       enable_direct_io;
    ext4_journal_state_t      journal_state;
    char                      source_path[1024];
    uint64_t                  partition_offset;
    uint64_t                  partition_size;
    char                      volume_label[32];
    struct ext4_extent_cache *extent_cache;
    struct ext4_overlay_ctx  *overlay;
} ext4_vfs_ctx_t;
```
- **ロック階層順序**: 常に **`mut_cs` (Overlay) → `ctx->rwlock`（外側） → `win32_io` の `cache_lock`（内側）**、および独立リーフの **`extent_cache->lock` / `overlay->map_lock`** とし、循環・逆順取得を禁止する。`ext4_extent_pread` 自体は `ref_count` 取得後にロックを一切保持せず `win32_io_pread_direct` を呼ぶ。

### 5.2 各操作モジュール責務一覧
| モジュール | 公開関数 | 取得ロック | 主な処理内容 |
| :--- | :--- | :--- | :--- |
| `op_init.c` | `ext4_vfs_mount`, `ext4_vfs_unmount`, `ext4_vfs_mkfs` | 初期化 / 排他破棄 | パーティション解決 → `ext4_probe_partition` → Dirty 安全分岐 → `ext2fs_open2` + `ext2fs_read_bitmaps`。`mkfs` は `EXT4_FEATURE_INCOMPAT_EXTENTS` / `64BIT` / `METADATA_CSUM` / 256B iノード / `lost+found` 付きのクリーン ext4 イメージを生成。 |
| `op_getattr.c` | `ext4_vfs_stat`, `ext4_vfs_stat_ino`, `ext4_vfs_set_times` | Shared / Exclusive | パス正規化・大小文字フォールバック解決、64bit サイズ、100ns `FILETIME` 相互変換、`FILE_ATTRIBUTE_*`（ドットファイル `HIDDEN`、RO 時 `READONLY`）算出。 |
| `op_readdir.c` | `ext4_vfs_readdir` | Shared | `ext2fs_dir_iterate2` で各エントリ名と `ext4_stat_t` をコールバック返却。 |
| `op_open.c` | `ext4_vfs_open`, `ext4_vfs_close_handle` | Shared | `ext4_file_handle_t` 確保・種別・RO 書き込み権限確認。 |
| `op_read.c` | `ext4_vfs_read`, `ext4_vfs_lower_read`, `ext4_vfs_get_extents` | Direct I/O Hit: ロックレス / Miss・通常: Shared | `--direct-io` かつ Lower RO 時のみ `extent_cache` + `ext4_extent_pread` で物理セクタ直読（既定・RW 時は `ext2fs_file_read`）。`ctx->overlay` 有効時は `overlay_vfs_read` へ委譲。 |
| `op_write.c` | `ext4_vfs_write` | Exclusive | `write_to_eof` 対応書き込み、エクステント拡張、`mtime`/`ctime` 更新。 |
| `op_create.c` | `ext4_vfs_create` | Exclusive | 親確認・重複検査・`ext2fs_new_inode`・256B iノード初期化（`EXT4_EXTENTS_FL`）・`ext2fs_link` 登録後に親 iノードを再読込して時刻更新（`ext2fs_expand_dir` 後の上書き防止）。 |
| `op_mkdir.c` | `ext4_vfs_mkdir`, `ext4_vfs_can_delete` | Exclusive / Shared | `ext2fs_mkdir` によるディレクトリ作成、および `.` と `..` 以外のエントリ不在検証 (`-ENOTEMPTY`)。 |
| `op_unlink.c` | `ext4_vfs_unlink` | Exclusive | 空チェック、`ext2fs_unlink`、リンクカウント 0 時の全ブロック解放（`ext2fs_punch`）および iノード解放。 |
| `op_rename.c` | `ext4_vfs_rename` | Exclusive | 種別整合性・子孫移動禁止・`replace_if_exists`・Windows 大小文字のみリネーム対応、ディレクトリ移動時の `..` 更新。 |
| `op_truncate.c` | `ext4_vfs_truncate` | Exclusive | `ext2fs_file_set_size2` による縮小（ブロック解放）およびスパース拡張。 |
| `op_symlink.c` | `ext4_vfs_symlink`, `ext4_vfs_readlink` | Exclusive / Shared | 60B 未満 Fast Symlink / 60B 以上ブロック格納リンクの作成と読取。 |
| `op_statfs.c` | `ext4_vfs_statfs`, `ext4_vfs_set_label` | Shared / Exclusive | ボリューム容量・シリアル番号・ラベル取得と更新（スーパーブロック CRC32C 再計算含む）。 |
| `op_flush.c` | `ext4_vfs_flush` | Exclusive | `ext2fs_flush` によるメタデータ・ビットマップ・I/O キャッシュ同期。 |

---

## 6. ダイレクト・エクステント ＆ Overlay 層詳細 (`src/extent/`, `src/overlay/`)

### 6.1 `src/extent/extent_map.h` / `.c`
- **有効条件**: `enable_direct_io != 0` かつ `lower_read_only != 0`（既定の RO マウントまたは `--overlay` で `--direct-io` 明示時のみ有効、`--rw` 時併用は `-EINVAL`）。
- **抽出対象 (`ext4_extent_extract`)**:
  - `EXT4_INLINE_DATA_FL` / 60B 未満 Fast Symlink: `EXT4_STORAGE_INLINE` としてバイト列を保持し、メモリから即返却。
  - `EXT4_EXTENTS_FL`: `ext2fs_extent_open2` + `EXT2_EXTENT_ROOT` → `while (!(e_flags & LEAF)) EXT2_EXTENT_DOWN` → `EXT2_EXTENT_NEXT_LEAF` で全リーフノードを走査し、連続物理エクステントを統合（`is_uninit` も正確に維持）。
  - 従来型間接ブロック: `ext2fs_block_iterate3(..., BLOCK_FLAG_READ_ONLY, ...)` で物理ブロックを結合。
- **参照・読み取り (`ext4_extent_resolve`, `ext4_extent_pread`)**:
  - 論理オフセットで昇順ソートされた `runs[]` を `O(log N)` 二分探索。スパースホールや `is_uninit` 領域は I/O 発行せず `memset(0)`、実ブロック領域は `win32_io_pread_direct` へ渡す。
- **キャッシュ (`ext4_extent_cache_t`)**:
  - 最大 4096 エントリの LRU ＋ 4096 バケットハッシュ表。各 `ext4_extent_map_t` は `InterlockedIncrement` / `InterlockedDecrement` による `ref_count` 管理を行い、evict と並列 Read の競合を防止する。

### 6.2 `src/overlay/overlay_vfs.h` / `.c`
- **基本原則 & Lower-ID 照合**: `cfg.overlay_upper_dir` 指定時、Lower (`ctx->fs`) は強制的に Read-Only でオープンし、すべての更新系操作は NTFS 上の `upper_dir` に向ける。`overlay_vfs_create` 時に `<upper_dir>\.wh.__ext4_lower_id`（UUID・総ブロック数・ブロックサイズ）を検査／記録し、異なる Lower ボリュームへの誤流用を `-EINVAL` でブロックする（未マーカーの非空ディレクトリに対しては警告ログを出力）。
- **解決規則 (`overlay_resolve`)**:
  - ルート `/` からパス要素ごとにたどり、`upper_dir` 上の `.wh.<name>`（ホワイトアウト）およびディレクトリ内 `.wh.__dir_opaque`（削除後再作成による不透過ディレクトリ）を判定した上で Upper 優先 → Lower フォールバックを行う。Upper に通常ファイルが存在する場合は同名の Lower ディレクトリおよび配下を遮蔽する。
- **iノード安定性 (`overlay_assign_ino` / `overlay_lookup_ino`)**:
  - Lower に由来するエントリには Lower の `ext2_ino_t` を引き継ぎ、Upper 新規作成エントリには `0x80000000U` 以降の合成 `ext2_ino_t` を割り当て、`path <-> ino` 双方向表で管理（`rename` 時もプレフィックス配下のパスを一括更新）。
- **Copy-up (`ensure_upper_copy`) & マージ列挙 (`overlay_vfs_readdir`)**:
  - Lower のみ存在するファイルへの `writable` オープン・`write`・`truncate`・`set_times`・`rename` 時に一時ファイル経由で Upper へ原子的コピーアップ（`ext4_vfs_lower_read` により Direct Extent 経由で高速転送）。
  - `readdir` は Upper と Lower を重複排除・ホワイトアウトフィルタ付きで統合し、`.wh.*` マーカーを透過的に隠蔽する。

---

## 7. エラーコード変換規則 (`src/common/ext4_errno.h` / `.c`)

| 発生事象 | `libext2fs` `errcode_t` / POSIX `-errno` | Windows `NTSTATUS` |
| :--- | :--- | :--- |
| エントリ不在 | `EXT2_ET_FILE_NOT_FOUND` / `-ENOENT` | `STATUS_OBJECT_NAME_NOT_FOUND` (`0xC0000034`) |
| 非ディレクトリ | `EXT2_ET_NO_DIRECTORY` / `-ENOTDIR` | `STATUS_NOT_A_DIRECTORY` (`0xC0000103`) |
| ディレクトリに対しファイル操作 | `EXT2_ET_IS_A_DIR` / `-EISDIR` | `STATUS_FILE_IS_A_DIRECTORY` (`0xC00000BA`) |
| 既存衝突 | `EXT2_ET_FILE_EXISTS` / `EXT2_ET_DIR_EXISTS` / `-EEXIST` | `STATUS_OBJECT_NAME_COLLISION` (`0xC0000035`) |
| 非空ディレクトリ削除 | `EXT2_ET_DIR_NOT_EMPTY` / `-ENOTEMPTY` | `STATUS_DIRECTORY_NOT_EMPTY` (`0xC0000101`) |
| RO 書き込み / Dirty 保護 | `EXT2_ET_RO_FILSYS` / `EXT2_ET_JOURNAL_UNSUPP_INCOMPAT` / `-EROFS` | `STATUS_MEDIA_WRITE_PROTECTED` (`0xC00000A2`) |
| 空き容量/iノード不足 | `EXT2_ET_BLOCK_ALLOC_FAIL` / `EXT2_ET_INODE_ALLOC_FAIL` / `-ENOSPC` | `STATUS_DISK_FULL` (`0xC000007F`) |
| 不正引数 | `EXT2_ET_INVALID_ARGUMENT` / `-EINVAL` | `STATUS_INVALID_PARAMETER` (`0xC000000D`) |
| EOF 到達 | Read `got == 0` | `STATUS_END_OF_FILE` (`0xC0000011`) |
| その他 I/O エラー | `EXT2_ET_SHORT_READ` / `EXT2_ET_SHORT_WRITE` / `-EIO` | `STATUS_DEVICE_DATA_ERROR` (`0xC000009C`) |

---

## 8. WinFsp ドライバ層詳細 (`src/driver/winfsp_driver.h`, `winfsp_driver.c`)

- **`FSP_FSCTL_VOLUME_PARAMS` 設定**:
  - `SectorSize = (UINT16)vol.block_size`、`SectorsPerAllocationUnit = 1`、`MaxComponentLength = 255`
  - `FileInfoTimeout = 1000`
  - `CaseSensitiveSearch = ctx->case_insensitive ? 0 : 1`、`CasePreservedNames = 1`、`UnicodeOnDisk = 1`
  - `PersistentAcls = 0`、`ReadOnlyVolume = ctx->read_only ? 1 : 0`、`SupportsPosixUnlinkRename = 1`
  - `FileSystemName = L"ext4"`
- **動的ロード (`FspTryLoadDll` / `FspGetRuntimeApi`)**:
  - `winfsp-x64.dll` → レジストリ `HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir` → `HKLM\SOFTWARE\WinFsp\InstallDir` → `%ProgramFiles(x86)%\WinFsp\bin\winfsp-x64.dll` → `%ProgramFiles%\WinFsp\bin\winfsp-x64.dll` の順で解決。
- **インプロセス検証サポート**:
  - `winfsp_driver_get_interface()`、`winfsp_driver_create_test_fs()`、`winfsp_driver_destroy_test_fs()` により、WinFsp ランタイム未導入環境でも全コールバック（`ReadDirectory` の `Marker` ページング、`ConstrainedIo`、`SetDelete` + `Cleanup` 等を含む）を単体・結合テスト可能としている。
