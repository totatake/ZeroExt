# テスト・検証ガイド

本書は **ZeroExt** の自動テストスイート（C/C++ `CTest`・`.NET` GUI テスト）、メモリ安全性／静的解析（ASan・CRT リーク検出・`/analyze`）、および実機動作検証の基準をまとめた検証ガイドです。

- インストール・日常の CLI/GUI 操作手順は [利用ガイド](guides/usage.md)、ビルド・保守手順は [開発・ビルド・保守ガイド](guides/development.md) を参照してください。
- 性能ベンチマーク測定および Perfetto トレース解析結果は [評価・解析レポート](reports/README.md) を参照してください。

---

## 1. 検証全体構成と実行方法

Windows 10 / 11 x64 環境（Visual Studio 2022 + Python 3.x + 任意で `.NET 10 SDK`）で以下を実行します（**WinFsp SDK はビルド・自動テスト実行時には不要**で、[winfsp.h](../third_party/winfsp/include/winfsp/winfsp.h) と `winfsp_driver_create_test_fs()` によるインプロセス検証に対応しています）：

```powershell
# 1. コア C11 ビルド + CTest 6 スイート + GUI ビルド + GUI 自動テスト 11 件 + --self-test
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1

# 2. C/CMake/CTest のみ（-SkipGui）
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_test.ps1 -Clean -SkipGui

# 3. GUI テスト単体実行
powershell -ExecutionPolicy Bypass -File .\scripts\build_gui.ps1

# 4. メモリ破壊・リーク・静的解析（ASan + Debug CRT リーク検出 + /analyze）
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1
```

CI（[.github/workflows/ci.yml](../.github/workflows/ci.yml)）およびリリースパッケージ生成（[.github/workflows/release.yml](../.github/workflows/release.yml)）では `windows-latest` 上で上記 1 と 4 を毎回実行しています。

---

## 2. C/C++ 単体・結合テスト (`ctest` / `zeroext_tests.exe`)

[tests/test_main.c](../tests/test_main.c) には 6 つの CTest スイート（計 **32 テスト関数**）が登録されています。`zeroext_tests.exe --suite <name>` でスイート単位の実行も可能です。

### 2.1 `win32_io` ([tests/test_win32_io.c](../tests/test_win32_io.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_4kb_rmw_and_alignment` | 4KB セクタ設定時の 1024B 非アライメント Write (RMW) と周辺バイト無破損 |
| `test_partition_boundary_guard` | パーティション `[offset, offset+size)` 境界外 Read/Write の拒否 (`EXT2_ET_LLSEEK_FAILED`) |
| `test_read_only_and_lru_cache` | RO モード書込拒否 (`EXT2_ET_RO_FILSYS`)、LRU キャッシュヒット計測 |
| `test_overlapped_concurrent_pread` | `FILE_FLAG_OVERLAPPED` + FLS イベントによる並列 `win32_io_pread_direct` のデータ整合性 |
| `test_sharded_cache_concurrent_eviction_and_invalidate` | 32 シャード CLOCK キャッシュの並列置換・参照・無効化競合検証 |

### 2.2 `partition_and_probe` ([tests/test_partition_and_probe.c](../tests/test_partition_and_probe.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_gpt_disk_and_embedded_ext4` | 保護 MBR + GPT ヘッダ解析、UTF-16LE パーティション名、内蔵 ext4 解析 |
| `test_mbr_and_ebr_logical_partitions` | MBR + 拡張 EBR チェーン解析（論理パーティション 5 の ext4 検出） |
| `test_dirty_journal_and_jbd2_probe` | JBD2 inode 8 スーパーブロック（Big-Endian `s_start != 0`）および `EXT2_ERROR_FS` 検出 |

### 2.3 `vfs_operations` ([tests/test_vfs_operations.c](../tests/test_vfs_operations.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_vfs_crud_large_extent_and_persistence` | `mkfs`, `mkdir`, >3MB ファイル書込/読込, `readdir`, `rename`, Fast/Block symlink, `truncate`, `unlink`, 再マウント永続性 & CRC32C 検証 |
| `test_fragmented_multi_level_extent_tree` | 断片化ファイルによるエクステントツリー深さ拡張 (`depth 0 -> 1`) の読み書き整合性 |
| `test_concurrent_parallel_reads` | 8 スレッド並列 `ext4_vfs_read` (`SRWLOCK` Shared) のデータ整合性 |
| `test_dirty_journal_safety_policy` | Dirty Journal 検出時の RW マウント拒否 (`-EROFS`) ＆ RO 自動フォールバック |
| `test_multiblock_directory_expansion` | 120 ファイル作成による `ext2fs_expand_dir` 複数ブロックディレクトリ拡張・部分削除・再マウント検証 |
| `test_gpt_partition_rw_isolation` | GPT Partition 2 への RW マウント書込時に Partition 1 の保護バイト (`0x5A`) が不変であること |
| `test_dentry_cache_positive_negative_and_invalidation` | 16 シャード正／負 Dentry キャッシュ登録・大文字小文字解決・変更時世代無効化検証 |
| `test_write_stat_preserves_extra_times_and_extents` | `ext4_vfs_write_stat` 単一パス更新時の 256B iノード拡張時刻およびエクステント整合維持 |
| `test_multi_group_backup_superblock_flush` | 複数ブロックグループ（140MB 規模）環境でのバックアップスーパーブロック／BGD フラッシュ安全検証 |
| `test_concurrent_rw_and_dentry_invalidation_stress` | 並列 RW アクセスと Dentry 世代無効化・`fs_lock` が混在する高負荷ストレステスト（ASan 対象） |
| `test_case_sensitive_coexistence` | `case_insensitive = 0` 時における `/Foo.txt` / `/foo.txt`・`/Sub` / `/sub` 共存・リネーム・削除・RO 再マウント永続性検証 |

### 2.4 `winfsp_callbacks` ([tests/test_winfsp_callbacks.c](../tests/test_winfsp_callbacks.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_winfsp_interface_e2e` | `FSP_FILE_SYSTEM_INTERFACE` 全主要コールバック（`GetVolumeInfo`, `SetVolumeLabel`, `GetSecurityByName`, `Create`, `Open`, `Overwrite`, `Read`/`EOF`, `Write`, `Flush`, `SetBasicInfo`, `SetFileSize`, `CanDelete`, `SetDelete`, `Rename`, `ReadDirectory` Marker ページング, `Cleanup`, `Close`） |
| `test_winfsp_volume_params_ro_cache_timeout` | RO マウント時 `FileInfoTimeout = INFINITE` と RW/Overlay 時有限タイムアウト設定の検証 |
| `test_winfsp_get_dir_info_by_name_and_large_readdir` | `PassQueryDirectoryFileName = 1` ＋ `FspGetDirInfoByName` 単一検索および多件数ディレクトリページネーション検証 |
| `test_winfsp_safe_unmount_target` | `zeroext unmount` 時のターゲット対象検証（非 ZeroExt / 非 WinFsp ドライブの誤解除抑止） |

### 2.5 `extent_and_overlay` ([tests/test_extent_and_overlay.c](../tests/test_extent_and_overlay.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_direct_extent_deep_tree_and_holes` | RW＋`--direct-io` および `--direct-io`＋`--no-direct-io` 矛盾指定拒否 (`-EINVAL`)、RW 自動無効、既定 RO（Direct I/O 自動有効）での 8 断片・多段エクステント木 (`eh_depth >= 1`) 抽出＆スパース横断非アライン直読と `--no-direct-io` 結果一致 |
| `test_extent_indirect_uninit_and_inline` | 既定 RO（Direct I/O ON）での従来型間接ブロック (`EXT4_STORAGE_INDIRECT`)、インラインデータ (`EXT4_INLINE_DATA_FL`)、未初期化エクステント (`is_uninit`) ゼロ埋め直読 |
| `test_overlay_vfs_and_persistence` | Dirty ext4 の RO 下位マウント＋既定 Direct I/O 併用、Copy-up 書込・追記・再マウント永続性、ホワイトアウト (`.wh.*`)・不透過ディレクトリ、別 UUID イメージに対する `.wh.__ext4_lower_id` 拒否 (`-EINVAL`)、下位非破壊検証 |
| `test_read_handle_cached_extents` | オープンハンドル (`ext4_file_handle_t.cached_extents`) 保持時のロックレス直読およびクローズ解放検証 |
| `test_overlay_case_sensitive_upperdir` | `case_insensitive = 0` + NTFS Upperdir での `FileCaseSensitiveInfo` 設定／拒否保護、`.wh.__ext4_lower_id` の `:cs` 混用ブロック検証 |

### 2.6 `perfetto_trace` ([tests/test_perfetto_trace.c](../tests/test_perfetto_trace.c))

| テスト関数 | 主な検証内容 |
| :--- | :--- |
| `test_trace_disabled_noop` | トレース無効時のゼロオーバーヘッド noop 動作検証 |
| `test_trace_vfs_direct_standard_and_overlay` | 4 スレッド並列標準読取・Direct I/O・Overlay Copy-up/Whiteout におけるロック待ち/保持・I/O・キャッシュカウンタ出力検証 |
| `test_trace_winfsp_callback_spans` | WinFsp 全コールバック境界スパン (`FspCreate`/`FspRead`/`FspWrite`/`FspReadDirectory` 等) 出力検証 |
| `test_trace_ring_wrap_and_reinit` | リングバッファラップ時のドロップ数計測と複数セッション再初期化 (`tls_epoch`) 検証 |

---

## 3. GUI 自動テスト (`ZeroExt.Gui.Tests` / `--self-test`)

[gui/ZeroExt.Gui.Tests/Program.cs](../gui/ZeroExt.Gui.Tests/Program.cs) に **11 件** の自動テストが実装されており、`scripts/build_gui.ps1` ではテスト実行に加えて実バイナリの `ZeroExtGui.exe --self-test` も検証します。

| テスト名 | 主な検証内容 |
| :--- | :--- |
| `JsonParsing_ScanResponse` | `zeroext list --json` 出力（物理ディスク・GPT/MBR・JBD2 状態）のデシリアライズ検証 |
| `JsonParsing_LsAndMountEvents` | `ls --json` および `--json-events` リアルタイム統計イベントパース検証 |
| `ArgBuilder_MountVariantsAndPreview` | Read-Only / `--overlay` / `--rw` / `--trace` 等に応じた `zeroext.exe` 引数組み立て検証 |
| `DriveLetterAndOverlayValidation` | ドライブレター空き判定・Upperdir 指定バリデーション検証 |
| `DriveLetter_SafeUnmountTarget` | GUI 側「残留ドライブ解除」における対象ボリューム判定（システムドライブ保護）検証 |
| `Localization_CompletenessAndSwitching` | `Strings.en.xaml` / `Strings.ja.xaml` 間のリソースキー網羅性および即時言語切替検証 |
| `MountErrorTranslator` | コアエラーコードのローカライズ翻訳メッセージ解決検証 |
| `SettingsService_RoundTrip` | `gui_settings.json` の設定・プリセット保存／復元往復検証 |
| `MainViewModel_ValidationAndPreview` | マウント前入力検証・状態遷移・CLI プレビュー更新検証 |
| `TrayIconService_MountUnmountMenuRebuild` | マウント／アンマウントに伴うシステムトレイメニュー再構築検証 |
| `LiveCoreCli_InspectAndBrowseIntegration` | 実 `zeroext.exe` サブプロセス起動による `inspect` / `ls --json` 疎結合統合検証 |

---

## 4. メモリ安全性・リーク・静的解析 (`Sanitizer` / `/analyze`)

C11 実装のメモリ破壊・解放漏れ・排他競合バグを継続的に検出するため、`CMakeLists.txt` のオプションおよび専用検証スクリプト [scripts/run_sanitizers.ps1](../scripts/run_sanitizers.ps1) を用意しています：

```powershell
# 3 種類すべて（ASan + CRT リークチェック + /analyze 静的解析）を一括実行
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1

# 個別実行（-Mode asan | leak | analyze）
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1 -Mode asan
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1 -Mode leak
powershell -ExecutionPolicy Bypass -File .\scripts\run_sanitizers.ps1 -Mode analyze
```

| チェック種別 | CMake オプション / ツール | 検出対象 | 補足 |
| :--- | :--- | :--- | :--- |
| **AddressSanitizer (ASan)** | `-DZEROEXT_ENABLE_ASAN=ON`<br>(`RelWithDebInfo` + `/fsanitize=address`) | ヒープ・スタック・グローバル境界外読み書き、Use-After-Free、二重解放 | `test_concurrent_rw_and_dentry_invalidation_stress` 等のマルチスレッドストレスを含む CTest 全 6 スイートを実行。 |
| **メモリリーク検出** | `-DZEROEXT_ENABLE_LEAK_CHECK=ON`<br>(`Debug` + `_CrtMemCheckpoint` / `_CrtMemDifference`) | テスト終了時に未解放のヒープブロック（1 バイト単位） | Windows MSVC 環境の ASan は LeakSanitizer 非対応のため、Debug CRT ヒープスナップショット比較でリーク時に CTest を失敗させます。 |
| **静的コード解析** | `-DZEROEXT_ENABLE_ANALYZE=ON`<br>(`cl.exe /analyze`) | NULL 逆参照、バッファ範囲外アクセス、未初期化変数、リソース・ロック不整合 | 自前 C ソース（`src/`・`tests/`）に対してビルド時に静的解析を実行します。 |

> [!NOTE]
> Windows x64 環境では GCC / Clang の ThreadSanitizer (`-fsanitize=thread`) が公式サポートされていないため、排他安全性については **ASan 有効下でのマルチスレッド競合ストレステスト**（`win32_io` シャードキャッシュ / `dentry_cache` 世代無効化 / RW 同時アクセス）および **`/analyze` 静的解析** により実効的な検証を行っています。

---

## 5. 実機 CLI / WinFsp 確認チェックリスト

1. **オフライン CLI 操作**: `zeroext.exe` で `mkfs` → `inspect` → `mkdir` → `write` → `cat` → `extents` → `mv` → `ls` → `rm` の一連操作を確認。
2. **WinFsp ドライブマウント & 並列負荷**: テストイメージへの WinFsp 経由 RO/RW マウント＋マルチスレッド並列アクセス、昇格環境で Explorer にドライブが表示されること（[ADR-0009](adr/0009-elevated-drive-letter-visibility.md)）、および `unmount` が非 WinFsp ドライブを誤解除しないことを確認。
3. **WinFsp Overlay マウント (`--overlay`)**: Lower ファイル読取・Upper 新規作成・Copy-up 書込後に Lower イメージが 1 バイトも変更されていないこと、および別 UUID イメージに対する `.wh.__ext4_lower_id` 拒否を確認。
4. **Upstream Zero-Patch 分離検証**: `third_party/e2fsprogs/` 配下に `_MSC_VER` / `_WIN32` / `windows.h` 等の Windows 固有記述が混入していないことを確認（[ADR-0005](adr/0005-zero-patch-e2fsprogs-compat-layer.md)）。
5. **非破壊ヘッダ生成確認（クロスプラットフォーム可）**: `python3 scripts/generate_e2fsprogs_headers.py ...` が警告・エラーなしで終了することを確認。
