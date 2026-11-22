# ADR-0008: Dirty Journal 検出時の安全機構（RW 拒否 / RO フォールバック）

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

`libext2fs` 単体は Linux カーネルの JBD2 ジャーナルトランザクション自動再生を行わないため、Linux 側が異常終了や休止状態で残した未コミットジャーナルを伴う ext4 ボリュームへそのまま書き込みを行うと、ファイルシステム破損やデータ消失を引き起こす危険がある。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: マウント時にジャーナル検査を行わず RW 許可
- **メリット**: 実装が単純。
- **デメリット / リスク**: `EXT3_FEATURE_INCOMPAT_RECOVER` や未適用トランザクションがある状態でメタデータ書換が走り、重大な FS 破損につながる。

### 案 B: マウント前の JBD2 検査＋RW 拒否（RO フォールバックオプション付）（採用）
- **メリット**: スーパーブロックの `EXT3_FEATURE_INCOMPAT_RECOVER` / `EXT2_ERROR_FS` およびジャーナル iノード (`s_journal_inum`) 先頭の JBD2 スーパーブロック（Magic `0xC03B3998`, `s_start != 0`, `s_errno != 0`）をマウント前に検査し、Dirty またはエラー検出時は RW マウントを拒否する。`ro_fallback_on_dirty` 指定時は安全に Read-Only へ自動切替し、`force_rw_on_dirty` 明示指定時のみ強制許可する。
- **デメリット / リスク**: 休止状態・異常終了後のボリュームに直接 RW マウントしたい場合は Linux 側での事前リカバリ（`e2fsck` / 再マウント）または Overlay 利用が必要となる。

## 3. 決定 (Decision)

**案 B（マウント前 JBD2 検査による RW 拒否と安全側フォールバック）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 意図しないデータ破損をマウント入口で確実に遮断できている。なお Read-Only マウントおよび `--overlay`（Lower RO 固定）時は未再生ジャーナルが存在しても安全に閲覧・救出できる。
- **受け入れるトレードオフ / 今後の制約**: マウント前検査ロジックと安全性ポリシーのテスト（`tests/test_partition_and_probe.c`, `tests/test_vfs_operations.c`）を常に維持する。
- **関連する変更箇所**: [src/disk/superblock_probe.c](../../src/disk/superblock_probe.c), [src/vfs/op_init.c](../../src/vfs/op_init.c)
