# ADR-0007: 2 階層 `SRWLOCK` によるマルチスレッド同期設計

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

WinFsp のディスパッチャおよび並列 `ReadFile` 負荷下では複数スレッドから同時に VFS コールバックが呼ばれる一方、`libext2fs` の内部キャッシュ・ビットマップ変更操作はスレッドセーフではないため、デッドロックのない排他設計が必要だった。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: VFS 全体を単一クリティカルセクション (`CRITICAL_SECTION`) で直列化
- **メリット**: 実装が単純で競合バグが出にくい。
- **デメリット / リスク**: 複数スレッドからの Read / Stat / Readdir まで直列化され、マルチコア環境のスループットが著しく低下する。

### 案 B: 2 階層 `SRWLOCK`＋ロック順序固定（採用）
- **メリット**:
  - `ext4_vfs_ctx_t` に `SRWLOCK rwlock` を置き、Read / Stat / ReadDir 系は共有ロック（`AcquireSRWLockShared`）で並列許可、Write / Create / Mkdir / Unlink / Rename / Truncate / Flush 系は排他ロック（`AcquireSRWLockExclusive`）で直列化できる。
  - `win32_io` のブロックキャッシュは独立の `SRWLOCK cache_lock` で保護する（後にシャード化＋Shared 参照へ拡張）。
  - デッドロック防止のためロック取得順序を **`ctx->rwlock` → `cache_lock`** の一方向に固定する（加えて `e2fsprogs 1.47.4` 対応後は `fs->icache` 保護用 `ctx->fs_lock` を `ctx->rwlock` の内側・`cache_lock` の外側に位置づける）。
- **デメリット / リスク**: 新規 VFS 関数・キャッシュ追加時にロック階層順を厳守する規律が求められる。

## 3. 決定 (Decision)

**案 B（階層化された `SRWLOCK` 同期と単一方向ロック順序の固定）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: メタデータ変更の安全性を保ちつつ高い並列読み取り性能とシャードキャッシュ最適化の土台を確立できた。
- **受け入れるトレードオフ / 今後の制約**: `ctx->rwlock` / `ctx->fs_lock` / `cache_lock` の逆順取得を禁止し、並列ストレステストおよび ASan で検証する。
- **関連する変更箇所**: [src/vfs/ext4_vfs.h](../../src/vfs/ext4_vfs.h), [src/vfs/](../../src/vfs), [src/io/win32_io.c](../../src/io/win32_io.c)
