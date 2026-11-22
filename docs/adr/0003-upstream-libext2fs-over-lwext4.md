# ADR-0003: `lwext4` ではなく本家 `e2fsprogs` (`libext2fs`) を採用する

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

ユーザー空間で ext2 / ext3 / ext4 を扱うコアライブラリとして、組み込み向け軽量ライブラリ（`lwext4`）と Linux 本家 `e2fsprogs`（`libext2fs`）のいずれを採用するか検討した。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: `lwext4` の採用
- **メリット**: コードベースが小さく Windows への移植箇所が比較的少ない。
- **デメリット / リスク**: 数 KB〜数十 KB のマイコン環境を主眼としているため固定・小規模ブロックキャッシュや単一全体ロックになりやすく、デスクトップ PC の潤沢な RAM とマルチコアを活用した大容量転送では性能が頭打ちになりやすい。

### 案 B: 本家 `e2fsprogs` (`libext2fs`) の採用（採用）
- **メリット**: 最新 Linux 環境が既定で有効化する `64bit`・`metadata_csum`（CRC32C）・HTree 大規模ディレクトリ・256B+ iノード（ナノ秒時刻・`crtime`）への完全追従性と信頼性に優れる。
- **デメリット / リスク**: POSIX / Autotools 前提のため Windows MSVC 向けの互換シム（`compat/`）が必要になる。

## 3. 決定 (Decision)

**案 B（本家 `libext2fs`）** をコアエンジンとして採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 現代的な ext4 機能との高い互換性と長期信頼性を確保できた（`v0.5.0` で `1.47.4` へ追従済み）。
- **受け入れるトレードオフ / 今後の制約**: ライセンスは GPLv2 となり、[ADR-0005](0005-zero-patch-e2fsprogs-compat-layer.md) に従う MSVC 互換層の維持が必要になる。
- **関連する変更箇所**: [third_party/e2fsprogs/](../../third_party/e2fsprogs)
