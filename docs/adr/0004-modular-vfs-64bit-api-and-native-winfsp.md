# ADR-0004: `fuse-ext2` 型モジュラー構造 × 64bit API × WinFsp ネイティブのハイブリッド設計

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

`libext2fs` の操作群をどのように構造化し、WinFsp へ接続するかのコードアーキテクチャを選定する必要があった。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 従来型 `fuse-ext2` のそのままの移植または FUSE 互換層経由
- **メリット**: FUSE コールバックと対応付けやすい。
- **デメリット / リスク**: `fuse-ext2` 由来コードには 32bit API 依存箇所が残り、`64bit` / `metadata_csum` 環境での RW 書き込みにリスクがある。FUSE 変換層を挟むと Windows 固有属性制御のオーバーヘッドが増える。

### 案 B: `fuse2fs` 型モノリシック構成 + FUSE 互換層
- **メリット**: `64bit` API を備える。
- **デメリット / リスク**: 単一巨大ソースに処理が集中し、操作単位の単体テストや責務分離が困難。

### 案 C: モジュラー VFS × 最新 64bit API × WinFsp ネイティブ（採用）
- **メリット**: `src/vfs/op_init.c`〜`op_flush.c` へ操作単位で分割することで責務と単体テストが明確化し、最新 64bit `libext2fs` API と `FSP_FILE_SYSTEM_INTERFACE` への直接接続により Windows 固有属性（`FILE_ATTRIBUTE_*`、100ns `FILETIME`、Delete-on-Close、`FileInfoTimeout`）を最小オーバーヘッドで制御できる。
- **デメリット / リスク**: WinFsp コールバック仕様に合わせた自前ブリッジ層（`src/driver/`）の実装が必要。

## 3. 決定 (Decision)

**案 C**（「操作別の `src/vfs/op_*.c` モジュール分割」×「最新 64bit `libext2fs` API」×「WinFsp ネイティブコールバック `FSP_FILE_SYSTEM_INTERFACE`」）を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 操作別モジュール境界が明確になり、WinFsp ランタイムなしでのインプロセス VFS/WinFsp コールバック単体テストが容易になった。
- **受け入れるトレードオフ / 今後の制約**: 新しい WinFsp 機能（例：Reparse Point 等）を追加する際は `winfsp_driver.c` と対応する `op_*.c` の双方をテストとセットで拡張する。
- **関連する変更箇所**: [src/vfs/](../../src/vfs), [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c)
