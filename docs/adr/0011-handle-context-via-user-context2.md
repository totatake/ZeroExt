# ADR-0011: `UmFileContextIsUserContext2 = 1` によるハンドル単位コンテキスト管理

- **ステータス**: Accepted
- **日付**: 2026-11-18
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

`UmFileContextIsUserContext2 = 0`（既定値）では WinFsp がオープンハンドルコンテキストを `FileNode` 単位の `UserContext` スロットに割り当てるため、並列アクセス時に同一パスに対する複数のクローズ要求が同一ポインタへ配送され、二重解放（`STATUS_HEAP_CORRUPTION` / `0xC0000374`）および解放済みポインタ参照が発生した。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: `UserContext` のまま参照カウント＋パス単位テーブルで共有管理
- **メリット**: WinFsp の既定動作のまま進められる。
- **デメリット / リスク**: `ext4_file_handle_t` が持つオープン固有属性（`create_options`, `delete_on_close`, `open_writable`）と参照カウント同期が複雑化し、クローズ時の競合バグを残しやすい。

### 案 B: `FSP_FSCTL_VOLUME_PARAMS.UmFileContextIsUserContext2 = 1` の有効化（採用）
- **メリット**: `FspOpen` / `FspCreate` が返す `ext4_file_handle_t *` を `UserContext2`（`FILE_OBJECT` / `FileDesc` 単位）として扱うことで、1 オープン = 1 クローズのライフタイムを WinFsp 側で厳密保証できる。
- **デメリット / リスク**: 特になし（構造体サイズがハンドル単位設計と完全に一致する）。

## 3. 決定 (Decision)

**案 B（`UmFileContextIsUserContext2 = 1` の常時有効化）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 並列アクセス環境での二重解放・解放済みポインタ参照が根本解消され、後に `ext4_file_handle_t` へのハンドル単位 `cached_extents` 保持（Step 3 最適化）も安全に実現できた。
- **受け入れるトレードオフ / 今後の制約**: `UmFileContextIsUserContext2 = 1` を不変条件として維持する。
- **関連する変更箇所**: [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c), [src/vfs/ext4_vfs.h](../../src/vfs/ext4_vfs.h)
