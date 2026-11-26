# ADR-0002: 独立プロジェクト (`ZeroExt4`) として分離開発する

- **ステータス**: Accepted
- **日付**: 2026-11-17（改定 2026-11-19）
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

既存の Read-Only ツール群（`LinuxRoDrive` 等）へ組み込むか、独立したリポジトリおよびビルド単位（`ZeroExt4`）として分離するかを決定する必要があった。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 既存 .NET / Read-Only ツール群への直接統合
- **メリット**: 単一リポジトリ・単一プロセスで機能をまとめられる。
- **デメリット / リスク**: 外部ドライバ不要・100% Read-Only の安全性モデルと、WinFsp 導入・`GENERIC_WRITE` による物理ディスク直接書込を伴うモデルが混在し安全境界が曖昧化する。.NET ソリューションへ C/CMake/`e2fsprogs` を混在させると保守コストが急増し、`libext2fs` の GPLv2 コピーレフトが全体へ波及する。

### 案 B: 独立リポジトリ・C/CMake プロジェクト（`ZeroExt4`）としての分離開発（採用）
- **メリット**: 安全性境界・ツールチェーン境界・ライセンス境界（GPLv2）を明確に分離でき、GUI 等とは CLI サブプロセス（JSON / Named Event IPC）で疎結合に連携できる。
- **デメリット / リスク**: コアバイナリ（`zeroext4.exe`）とフロントエンド（`ZeroExt4Gui.exe`）の間で IPC プロトコル設計が必要になる。

## 3. 決定 (Decision)

**案 B** を採用し、独立した C/CMake プロジェクト（`ZeroExt4`）として開発する。GUI フロントエンド等からは `zeroext4.exe` をサブプロセスとして呼び出し、ネイティブリンクを行わない。

## 4. 結果と影響 (Consequences)

- **良い影響**: `ZeroExt4Gui.exe` を含む利用層へ GPLv2 を直接波及させず、安全モデルとビルド構成を明快に分離できている。
- **受け入れるトレードオフ / 今後の制約**: CLI の `--json` / `--json-events` / `--stop-event` 契約を維持する必要がある。
- **関連する変更箇所**: [CMakeLists.txt](../../CMakeLists.txt), [gui/ZeroExt4.Gui/](../../gui/ZeroExt4.Gui), [LICENSE](../../LICENSE)
