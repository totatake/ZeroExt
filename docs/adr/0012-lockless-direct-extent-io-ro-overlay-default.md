# ADR-0012: RO / Overlay 既定有効・RW 自動無効のロックレス・エクステント直結 I/O (`src/extent/`, `--no-direct-io`)

- **ステータス**: Accepted
- **日付**: 2026-11-18（改定 2026-11-19）
- **関連文書**: [アーキテクチャ設計書](../architecture.md), [性能評価報告書](../reports/performance-evaluation.md)

## 1. 背景と課題 (Context)

`libext2fs` 標準の `ext2fs_file_read()` は 4 KB ブロック単位でメタデータ参照とブロックキャッシュロックを伴うため、大容量モデルの `mmap` や実物理 NVMe SSD（`FILE_FLAG_NO_BUFFERING`）への Cold 連続読み取りで帯域が大きく低下する問題があった。一方で、直接 RW モード下での物理セクタ直読は `libext2fs` 内部バッファ（`file->buf`）との Read-After-Write 不整合を招き得る。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 常に `--direct-io` を明示オプトインとする / または `--rw` でも一律有効化する
- **メリット / デメリット**: オプトイン方式では通常ユーザーが既定速度（数十分の 1）の低速経路のまま使ってしまい実用体験が劣る。一方 `--rw` への一律適用は、エクステント更新中間状態や `libext2fs` 未フラッシュバッファとの不整合によるデータ破損リスクが高い。

### 案 B: RO / `--overlay` 時のみ既定有効（`--no-direct-io` で無効化可）、`--rw` 時は自動無効・併用拒否（採用）
- **メリット**:
  1. Lower が Read-Only の場合（既定の RO マウントおよび `--overlay`）は安全性を損なわず、`O(log N)` 二分探索＋ロックレス並列 `win32_io_pread_direct` により実物理 NVMe SSD で `64.0 MB/s` → `1,563.7 MB/s` の大幅高速化を初期状態で提供できる（多段エクステント・間接ブロック・インラインデータ・未初期化エクステント・スパースゼロ埋めにも対応）。
  2. 非 Overlay `--rw` マウント時は自動的に無効化（`extent_cache` 未生成）し、`--direct-io` の明示併用は `-EINVAL` で拒否することで、書き込み時のデータ整合性を構造的に守る。
- **デメリット / リスク**: `--rw` マウントでは Direct I/O による高速化は適用されない。

## 3. 決定 (Decision)

**案 B（Lower Read-Only 限定での既定有効化、RW 自動無効化・明示併用拒否）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 大容量 AI モデル共有等の主要ユースケースにおいて設定不要で最高性能と Read-Only 安全性を両立できた。
- **受け入れるトレードオフ / 今後の制約**: 将来 `--rw` 性能を改善する場合も、物理セクタバイパスは解禁せず `libext2fs` 標準経路内の最適化（[ロードマップ](../roadmap.md)）で対処する。
- **関連する変更箇所**: [src/extent/extent_map.h](../../src/extent/extent_map.h), [src/extent/extent_map.c](../../src/extent/extent_map.c), [src/vfs/op_read.c](../../src/vfs/op_read.c), [tests/test_extent_and_overlay.c](../../tests/test_extent_and_overlay.c)
