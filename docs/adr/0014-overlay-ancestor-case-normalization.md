# ADR-0014: NTFS Overlay における祖先パス成分のケース正規化

- **ステータス**: Accepted
- **日付**: 2026-11-29
- **関連文書**: [ADR-0013](0013-ntfs-overlay-mount-and-lower-id-guard.md), [詳細設計書 §6.2](../design/detailed-design.md#62-srcoverlayoverlay_vfsh--c--overlay_toolsh--c), [利用ガイド §3.3](../guides/usage.md#33-シンボリックリンクと-windows-名前空間ガードの仕様)

## 1. 背景と課題 (Context)

Windows 環境のアプリケーションやシェルは、大文字小文字を区別しない（既定 CI モードの）ボリュームへアクセスする際、ディレクトリやファイルへのパスの一部または全成分をすべて大文字（例: `/HOME/USER/DOCUMENTS/STRATA/file.txt`）で渡してくることがある。

従来の `overlay_resolve_impl()`（[src/overlay/overlay_vfs.c](../../src/overlay/overlay_vfs.c)）は、最終成分（leaf）に限り既存の Upperdir エントリの実表記を参照して補正していたものの、**祖先ディレクトリ成分については要求表記のまま `cur` を積み上げ、未作成の NTFS Upperdir ディレクトリを作成**していた。NTFS は `CasePreservedNames = 1` のため、初回書き込みで一度大文字ディレクトリが Upperdir に作られると以降 `overlay_vfs_readdir`（Explorer 表示）・`overlay status` / `diff` / `export`（`.tar` パス）まで大文字表記が固定されてしまい、Lower ext4 上の実表記（例: `/home/user/Documents/Strata`）と乖離する問題があった。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: ファイルシステム側で変換せず運用回避（`--case-sensitive` 利用・手作業修正）
- **メリット**: 実装変更が不要。
- **デメリット / リスク**: Lower が混合ケースである限り大文字パスは別ツリー扱いまたは別ディレクトリ作成を招き、NTFS 上で子を持つディレクトリのケースのみリネームは Win32 上でも制限があるため後から容易に直せない。

### 案 B: `overlay_resolve` 走査時に全成分を一律小文字化・または要求表記を盲目採用
- **メリット**: 実装が単純。
- **デメリット / リスク**: Lower 上には `Documents`・`Downloads` のような初字大文字と `.config`・`src` のような小文字が混在するため、一律小文字化は POSIX の実表記を破壊し、`CasePreservedNames = 1` の原則に反する。

### 案 C: `overlay_resolve` のセグメント走査で 3 段優先順位により全祖先・既存 leaf を正規化（採用）
- **メリット**:
  1. `overlay_resolve_impl()` がルート `/` から各セグメントを走査する際、以下の優先順位（`CasePreservedNames` 準拠）で実表記を決定し、`cur`・`res->upper_path_w`・`out_norm` を正規化する。
     1. **Upperdir の NTFS 保存表記**（`overlay_get_upper_actual_leaf`）
     2. **Lower ext4 のディスク実表記**（`ext4_vfs_lower_stat_with_name`）
     3. **呼び出し元の要求表記**（Upper/Lower 双方に未存在の新規成分、および `overlay_vfs_rename` の宛先 leaf）
  2. ロック階層（`mut_cs -> ctx->rwlock -> ctx->fs_lock`）や inode 安定性（`ov_hash_path` は CI 時に小文字正規化してハッシュ、`lower_ino` 照合は不変）を壊さずに全更新操作（`create`/`mkdir`/`ensure_upper_copy`/`unlink`/`rename`）へ統一的に伝播する。
- **デメリット / リスク**: CI モード時のみ `ext4_vfs_lower_stat_with_name` を祖先階層数分呼び出すが、16 シャード `dentry_cache` により実 I/O は最小限に抑えられる。また、本修正以前に大文字表記で作られた既存 Upperdir ディレクトリは自動リネームされない。

## 3. 決定 (Decision)

**案 C（`overlay_resolve` における 3 段優先順位の祖先・leaf ケース正規化）** を採用する。

- すべての更新系パス解決は `overlay_resolve_ex(..., canonicalize_leaf = 1, ...)` を通し、Upper NTFS ＞ Lower ext4 ＞ 要求表記の順で `cur` を正規化する。
- `overlay_vfs_rename()` の宛先パスのみ `canonicalize_leaf = 0` を用い、祖先ディレクトリは実表記へ正規化しつつ、変更先ファイル／ディレクトリ名のケースのみ変更（例: `a.txt -> a.TXT`）は呼び出し元の要求表記を保持する。
- 同一親ディレクトリ内に Lower 上の大文字小文字衝突（例: `Sub` と `sub`）がある場合は、既存の決定論的タイブレーカー（厳密一致 → 辞書順最小名 → 最小 inode）で選ばれた表記へ Upperdir を固定する。

## 4. 結果と影響 (Consequences)

- **良い影響**: アプリケーションが大文字パスでオープン・作成しても、Upperdir のディレクトリ構造・Explorer 表示・`.wh.*` ホワイトアウト・`overlay diff/export` のパスが Lower ext4 と一致した実表記で維持される。
- **受け入れるトレードオフ / 運用上の注意**: 本修正前に大文字ディレクトリが作成済みの既存 Upperdir は、必要に応じて `zeroext4 overlay reset --upper <dir> --yes` で Upperdir を初期化し再作成する必要がある（Lower ext4 は無傷）。
- **関連する変更箇所**: [src/overlay/overlay_vfs.c](../../src/overlay/overlay_vfs.c), [tests/test_extent_and_overlay.c](../../tests/test_extent_and_overlay.c)
