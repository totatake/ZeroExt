# ADR-0009: 管理者権限実行時のドライブレター可視性（Mount Manager 優先 + デスクトップユーザーセッションへのフォールバック）

- **ステータス**: Accepted
- **日付**: 2026-11-17
- **関連文書**: [アーキテクチャ設計書](../architecture.md)

## 1. 背景と課題 (Context)

Windows で物理ディスク（`\\.\PhysicalDriveN`）を開くには昇格済み管理者トークンが必要となる。一方、WinFsp の `FspFileSystemSetMountPoint(fs, L"X:")` は（`LOCAL_SYSTEM` 以外では）内部で `DefineDosDeviceW` を呼び、ドライブレターを**呼び出し元ログオンセッションの `DosDevices`** にのみ作成する。UAC の分割トークン環境では昇格セッションと非昇格 Explorer のログオン ID が異なるため、「マウント成功と表示されるのに Explorer にドライブが現れない」現象が発生した。

## 2. 検討した選択肢 (Alternatives Considered)

### 案 A: 非昇格ユーザーに再度 `subst` / 手動定義させる、またはドキュメントで注意喚起のみ行う
- **メリット**: 追加コードが不要。
- **デメリット / リスク**: GUI アプリ（Explorer や通常のアプリ）からドライブが見えず実用性が著しく損なわれる。

### 案 B: `\\.\X:`（Mount Manager 経由グローバル登録）を優先し、失敗時はシェルセッション偽装で補完（採用）
- **メリット**:
  1. マウントポイントがドライブレター (`X:`) かつ昇格済みの場合、まず WinFsp が公式サポートする `\\.\X:` 形式で `SetMountPoint` を呼び、**Mount Manager 経由のグローバルドライブレター**（全セッションから可視）として登録する。
  2. これが失敗した場合は `X:` で通常マウントしたうえで、シェル (`GetShellWindow` → `explorer.exe`) のトークンを複製して `ImpersonateLoggedOnUser` し、**デスクトップユーザーのセッションにも** `DefineDosDeviceW(DDD_RAW_TARGET_PATH, "X:", \Device\Volume{...})` を定義する（アンマウント時も同じ偽装下で `DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE` により削除）。
  3. いずれの場合も `SHChangeNotify(SHCNE_DRIVEADD / SHCNE_DRIVEREMOVED)` で Explorer に通知する。
- **デメリット / リスク**: トークン複製・偽装まわりの権限制御とアンマウント時クリーンアップを正確にテストする必要がある。

## 3. 決定 (Decision)

**案 B（Mount Manager 優先 ＋ シェルセッション偽装フォールバック）** を採用する。非昇格実行時（イメージファイル `.img` のマウントなど）は従来どおり `X:` をそのまま使用する。

## 4. 結果と影響 (Consequences)

- **良い影響**: 管理者権限ターミナルや昇格版 `ZeroExt4Gui.exe` から物理ディスクをマウントした場合でも、非昇格 Explorer や通常アプリから即座にドライブを参照できる。
- **受け入れるトレードオフ / 今後の制約**: トークン偽装対象やアンマウント時削除の安全境界（[WinFsp ターゲット検証](../../src/driver/winfsp_driver.c)）を慎重に維持する。
- **関連する変更箇所**: [src/driver/winfsp_driver.c](../../src/driver/winfsp_driver.c)
