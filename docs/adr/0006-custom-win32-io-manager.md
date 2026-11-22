# ADR-0006: カスタム `struct_io_manager` (`win32_io_manager`) 設計

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

`e2fsprogs` 標準の `unix_io.c` は POSIX ファイルディスクリプタを前提としており、Windows の生ディスクハンドル（`\\.\PhysicalDriveX`）が要求する物理セクタ境界・バッファアライメント制約やパーティション範囲境界保護に対応できない。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: CRT `_open` / `_read` / `_lseeki64` ベースで `unix_io.c` を移植
- **メリット**: POSIX I/O との差分が小さい。
- **デメリット / リスク**: Windows 生ディスクの `FILE_FLAG_NO_BUFFERING` / 4KB セクタ境界・`_aligned_malloc` 要件を満たせず `ERROR_INVALID_PARAMETER` で失敗する。ファイルポインタ共有による並列競合や誤ったパーティション越境アクセスを構造的に防ぎにくい。

### 案 B: `CreateFileW` + `OVERLAPPED` + セクタ境界 RMW + 境界ガードを備えたカスタム `win32_io_manager`（採用）
- **メリット**:
  1. 512B/4096B セクタ境界と非アライメント I/O の Read-Modify-Write (RMW) を I/O 層で吸収し、上位 FS ロジックを簡潔に保てる。
  2. `OVERLAPPED` の明示 64bit オフセット指定によりファイルポインタ共有競合を排除でき、並列 Read を安全化できる。
  3. `[partition_offset_bytes, partition_offset_bytes + partition_size_bytes)` の厳密クランプにより、同一ディスク上の Windows パーティションや GPT テーブルへの誤書込を物理層で防止できる。
- **デメリット / リスク**: I/O マネージャとスレッドセーフキャッシュを独自にテスト・保守する必要がある。

## 3. 決定 (Decision)

**案 B（カスタム `win32_io_manager`）** を実装し、`unix_io.c` を置き換える。

## 4. 結果と影響 (Consequences)

- **良い影響**: 生ディスクと `.img` 双方に対する安全性（境界保護）と並列非同期 I/O 基盤を両立できた。
- **受け入れるトレードオフ / 今後の制約**: RMW・境界ガード・キャッシュ置換の単体テスト（`tests/test_win32_io.c`）を常に維持する。
- **関連する変更箇所**: [src/io/win32_io.h](../../src/io/win32_io.h), [src/io/win32_io.c](../../src/io/win32_io.c), [tests/test_win32_io.c](../../tests/test_win32_io.c)
