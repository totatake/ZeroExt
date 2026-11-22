# ADR-0005: `e2fsprogs` 追従性を担保する Zero-Patch 互換レイヤー設計

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

上流 `e2fsprogs` は GCC / POSIX / Autotools 前提で書かれているため MSVC C11 でそのままではビルドできないが、`third_party/e2fsprogs/` へ直接 Windows 向けパッチを多数書き込むと上流バージョン追従時のマージ負荷が高まる。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: `third_party/e2fsprogs/` 各ファイルへの手動 `#ifdef _MSC_VER` / `#ifdef _WIN32` パッチ適用
- **メリット**: 初回のビルド通し自体は単純。
- **デメリット / リスク**: 上流更新のたびに大量の差分確認・再パッチ適用・コンフリクト解消が必要になる。

### 案 B: `compat/` + 自動ヘッダ生成 + 再現スクリプトによる Zero-Patch 分離構成（採用）
- **メリット**: CMake `/FI`（`compat/msvc_compat.h` / `compat/msvc_public_compat.h`）・POSIX スタブ（`compat/include/`）・`scripts/generate_e2fsprogs_headers.py` に互換コードを局所化できる。MSVC C11 構文制約で避けられない最小調整も `scripts/update_e2fsprogs.ps1` に再現可能コードとして閉じ込め、将来のアップストリーム更新を自動化できる。
- **デメリット / リスク**: CMakeLists の `/FI` ターゲットスコープ管理（警告抑制の `PRIVATE` / `INTERFACE` 分離）を正確に保つ必要がある。

## 3. 決定 (Decision)

**案 B**（Zero-Patch 互換レイヤー構成）を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 実際に `v0.5.0` で `e2fsprogs` を `1.47.4` へ最小工数・自動検証付きで追従できた。
- **受け入れるトレードオフ / 今後の制約**: `third_party/e2fsprogs/` への手動 Windows 固有パッチ混入を禁止し、`scripts/update_e2fsprogs.ps1` を唯一の更新入口として維持する。
- **関連する変更箇所**: [compat/](../../compat), [scripts/generate_e2fsprogs_headers.py](../../scripts/generate_e2fsprogs_headers.py), [scripts/update_e2fsprogs.ps1](../../scripts/update_e2fsprogs.ps1), [CMakeLists.txt](../../CMakeLists.txt)
