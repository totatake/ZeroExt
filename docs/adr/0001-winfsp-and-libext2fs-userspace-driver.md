# ADR-0001: 純粋カーネルドライバ (`.sys`) ではなく `WinFsp + libext2fs` を採用する

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

Windows 上から Linux の ext2 / ext3 / ext4 ボリュームをドライブレターとして透過的に読み書きするには、Windows ファイルシステムドライバ（FSD）としての実装方式を決定する必要があった。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 純粋な Windows カーネルドライバ (`.sys`) での自作
- **メリット**: 外部ランタイム（WinFsp）不要で単体動作させられる余地がある。
- **デメリット / リスク**: `libext2fs` が標準 C ライブラリ（`malloc`, `errno`, POSIX I/O）を前提としており、`ntoskrnl.exe` 内の IRQL・ページング制約環境へそのまま移植・リンクすることが困難。Cache Manager (`Cc`) / Memory Manager (`Mm` / `mmap`) 連携の実装コストが高く、バグ発生時に即 BSOD を引き起こし、Windows 11 向けの EV コード署名および Microsoft Attestation Signing も必須となる。

### 案 B: `WinFsp + ユーザー空間 libext2fs`（採用）
- **メリット**: MS 署名済みの `winfsp-x64.sys` が Windows Cache Manager (`Cc`) / Memory Manager (`Mm` / ページング I/O / `mmap`) を代行するため、大容量 Read や `mmap` でも高速・安全に動作する。ユーザー空間プロセスのためバグで BSOD を起こさず、Attestation Signing も不要。
- **デメリット / リスク**: 実ドライブマウントの実行時には WinFsp ランタイムの導入が必要となる。

## 3. 決定 (Decision)

**案 B（WinFsp をカーネルプロキシとしたユーザー空間ドライバ）** を採用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: BSOD リスクを構造的に排除したうえで、`libext2fs` の堅牢な ext2/3/4 実装と Windows NT キャッシュ協調を両立できた。
- **受け入れるトレードオフ / 今後の制約**: 実ドライブマウント時には WinFsp ランタイムが必要（ただしビルド時および `list` / `inspect` / `ls` 等のオフライン操作は WinFsp 不要）。
- **関連する変更箇所**: [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c)
