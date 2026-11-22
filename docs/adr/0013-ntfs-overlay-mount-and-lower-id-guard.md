# ADR-0013: NTFS を Upper 層とする非破壊 Overlay マウント (`src/overlay/`) と Lower-ID 照合

- **ステータス**: Accepted
- **日付**: 2026-11-18（改定 2026-11-19）
- **関連文書**: [アーキテクチャ設計書](../architecture.md), [詳細設計書](../design/detailed-design.md)

## 1. 背景と課題 (Context)

デュアルブート環境の Linux パーティションや組み込み `.img` へ Windows 側からアクセスする際、キャッシュ・設定ファイル・一時出力の書き込みが必要になっても、元の ext4 ボリュームを 1 バイトも書き換えたくない要求が多い。同時に、既存の NTFS Upperdir を誤って別の ext4 ボリュームへ再利用すると古いホワイトアウトや Copy-up ファイルが意図せず適用される危険がある。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 通常の Read-Write マウントのみ、または Upperdir 識別子チェックなしの Overlay
- **メリット**: 実装が単純。
- **デメリット / リスク**: 直接 RW は Linux 環境破損リスクを伴う。また Upperdir 識別子がないと別ディスク／別イメージに対して `.wh.<name>` や `.wh.__dir_opaque` が誤適用される。

### 案 B: Lower 厳密 RO 固定＋NTFS Upperdir＋`.wh.__ext4_lower_id` 拘束検証（採用）
- **メリット**:
  1. `--overlay <upper_ntfs_dir>` 指定時は Lower ext4 を常に厳密 `GENERIC_READ`（Dirty Journal ボリュームでも安全オープン可）＋Direct I/O 既定 ON に固定し、書き込み・Copy-up・ホワイトアウト削除（`.wh.*`, `.wh.__dir_opaque`）を NTFS Upperdir へ逃がす。
  2. Upperdir ルートに隠しマーカー `.wh.__ext4_lower_id`（Lower の `s_uuid` + 総ブロック数 + ブロックサイズ）を記録し、別 Lower との不一致を検出した場合はマウントを `-EINVAL` で拒否する（未設定の非空ディレクトリには警告ログ出力後に記録、将来 Phase 1 で厳格拒否フラグ化を検討）。
- **デメリット / リスク**: 大規模ディレクトリの Lower 側リネーム時は Upper へ再帰 Copy-up する設計上のコストがあり、GUI/CLI での差分可視化・保護強化が継続課題となる。

## 3. 決定 (Decision)

**案 B（NTFS Upperdir Overlay ＋ `.wh.__ext4_lower_id` 整合チェック）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: AI モデル共有や `.img` 検証において ext4 非破壊性と実用的な読み書き運用を両立できた。
- **受け入れるトレードオフ / 今後の制約**: 非空・未マーカー Upperdir の扱いやクローン `.img` 識別のさらなる厳格化は [ロードマップ §4.2](../roadmap.md#42-overlay-upperdir-安全性lower-識別子ガードの強化) で段階的に強化する。
- **関連する変更箇所**: [src/overlay/overlay_vfs.h](../../src/overlay/overlay_vfs.h), [src/overlay/overlay_vfs.c](../../src/overlay/overlay_vfs.c), [tests/test_extent_and_overlay.c](../../tests/test_extent_and_overlay.c)
