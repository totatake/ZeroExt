# ADR-0010: WinFsp ABI 構造体レイアウトの検証とコンパイル時固定

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

ZeroExt はビルド環境に WinFsp SDK の事前インストールを要求しないため、WinFsp 2.x の FSD ABI 定義を自前ヘッダ `third_party/winfsp/include/winfsp/winfsp.h` に保持し、実行時に `winfsp-x64.dll` を動的ロードしている。初期定義では `AllowOpenInKernelMode` 以降のビットフィールド位置と `FSP_FSCTL_VOLUME_PARAMS` 末尾サイズ（512 bytes vs 公式 504 bytes）が公式 WinFsp ABI と食い違っており、`Version` に誤ったサイズを渡す問題が発生した。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: WinFsp SDK インストールを必須化しヘッダ取り込みを廃止
- **メリット**: 手動で ABI 定義を維持しなくて済む。
- **デメリット / リスク**: ローカル環境や GitHub Actions CI (`windows-latest`) で外部 MSI 事前導入が必要になり、ビルド可搬性（`NFR-04`）とインプロセス検証性が低下する。

### 案 B: 自前ヘッダを公式 WinFsp 2.x ABI と照合し、`FSP_ABI_ASSERT` でコンパイル時固定（採用）
- **メリット**: `FSP_FSCTL_VOLUME_PARAMS` (`504 bytes`)、`FSP_FSCTL_FILE_INFO` (`72 bytes`)、`FSP_FSCTL_DIR_INFO` (`FileNameBuf` @ `0x68`) を公式 WinFsp 2.x ABI 仕様と厳密照合し、コンパイル時静的アサーション (`FSP_ABI_ASSERT`) で構造体サイズ・主要フィールドオフセットの退行をビルド時に即検知できる。
- **デメリット / リスク**: 新しい WinFsp 構造体フィールドを有効化する際は公式定義とのオフセット検証・`FSP_ABI_ASSERT` 追記が必要になる。

## 3. 決定 (Decision)

**案 B（自己完結型 WinFsp ABI ヘッダ ＋ コンパイル時 `FSP_ABI_ASSERT` 検証）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: WinFsp SDK 未導入環境や CI でも安定ビルド・インプロセス検証が可能になり、ABI 不一致によるカーネルドライバへのパラメータ誤伝達を防げる。
- **受け入れるトレードオフ / 今後の制約**: WinFsp 関連の構造体変更時は必ず公式 WinFsp ABI（構造体サイズ・オフセット）を照合しアサーションを更新する。
- **関連する変更箇所**: [third_party/winfsp/include/winfsp/winfsp.h](../../third_party/winfsp/include/winfsp/winfsp.h), [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c)
