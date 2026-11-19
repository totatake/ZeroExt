using System;
using System.Collections.Generic;
using ZeroExt.Gui.Models;

namespace ZeroExt.Gui.Services
{
    public sealed class TranslatedMountError
    {
        public string Title { get; init; } = string.Empty;
        public string Message { get; init; } = string.Empty;
        public string Suggestion { get; init; } = string.Empty;
        public string RawDetail { get; init; } = string.Empty;
    }

    public static class MountErrorTranslator
    {
        public static TranslatedMountError Translate(
            MountEventMessage? errorEvent,
            IReadOnlyList<string> stderrLines,
            int? exitCode = null)
        {
            string combined = string.Join("\n", stderrLines ?? Array.Empty<string>());
            string eventMsg = errorEvent?.Message ?? string.Empty;
            string allText = string.IsNullOrEmpty(eventMsg)
                ? combined
                : (string.IsNullOrEmpty(combined) ? eventMsg : eventMsg + "\n" + combined);
            int? code = errorEvent?.Code;

            if (allText.Contains("belongs to a different lower ext4 filesystem", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains(".wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase))
            {
                return new TranslatedMountError
                {
                    Title = "Overlay Upperdir の識別子が一致しません",
                    Message = "指定された NTFS 書き込み先フォルダ (.wh.__ext4_lower_id) は、別の ext4 ファイルシステムで使用されたフォルダです。",
                    Suggestion = "データ混在を防ぐため、新しい空のフォルダを指定するか、該当の Upperdir を別の場所に切り替えてください。",
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Refusing RW mount on dirty ext4", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("without --force-dirty", StringComparison.OrdinalIgnoreCase) ||
                code == -30 /* -EROFS */)
            {
                return new TranslatedMountError
                {
                    Title = "未再生ジャーナル (Dirty) のため RW マウントを拒否しました",
                    Message = "対象の ext4 ボリュームに未再生の JBD2 ジャーナルまたはエラーフラグが検出されたため、データ破損防止のため書き込みマウントをブロックしました。",
                    Suggestion = "「Read-Only（推奨）」または「Overlay（非破壊書き込み）」モードを使用するか、詳細オプションの「Dirty 時は自動で Read-Only にフォールバック」を有効にしてください。",
                    RawDetail = allText,
                };
            }

            if (allText.Contains("WinFsp runtime", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("winfsp-x64.dll", StringComparison.OrdinalIgnoreCase) ||
                code == -19 /* -ENODEV */)
            {
                return new TranslatedMountError
                {
                    Title = "WinFsp ランタイムが見つかりません",
                    Message = "ドライブとしてマウントするには WinFsp (Windows File System Proxy) のインストールが必要です。",
                    Suggestion = "上部の「WinFsp ガイド」から WinFsp をインストール（winget install WinFsp.WinFsp）したあと再実行してください。ファイル閲覧だけなら「ファイルブラウザ」タブでも利用できます。",
                    RawDetail = allText,
                };
            }

            if (allText.Contains("0xC0000022", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("0xC0000035", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("SetMountPoint failed", StringComparison.OrdinalIgnoreCase))
            {
                return new TranslatedMountError
                {
                    Title = "マウント先ドライブまたはフォルダを使用できません",
                    Message = "指定したドライブレターが既に使用中（カードリーダーや残留ドライブ等）か、マウント先フォルダが空ではない可能性があります。",
                    Suggestion = "別の空いているドライブレターを選択してください。過去の異常終了でドライブ文字が残っている場合は「ツール → 残留ドライブ解除」から解除できます。",
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Access Denied", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("アクセスが拒否", StringComparison.OrdinalIgnoreCase) ||
                code == -13 /* -EACCES */)
            {
                return new TranslatedMountError
                {
                    Title = "ディスクへのアクセスが拒否されました",
                    Message = "物理ディスク（\\\\.\\PhysicalDriveX）または指定パスへのアクセス権限が不足しています。",
                    Suggestion = "物理ディスクをマウントする場合は上部の「管理者権限で再起動」ボタンから昇格して再実行してください。",
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Failed to mount", StringComparison.OrdinalIgnoreCase) &&
                (code == -2 /* -ENOENT */ || code == -22 /* -EINVAL */))
            {
                return new TranslatedMountError
                {
                    Title = "対象のパーティションまたはファイルシステムを開けません",
                    Message = "指定したソースファイル、パーティション番号、または ext2/3/4 スーパーブロックを読み取れませんでした。",
                    Suggestion = "ソースのパスとパーティション番号が正しいか、「再スキャン」で最新状態を確認してください。",
                    RawDetail = allText,
                };
            }

            string codeSuffix = code.HasValue
                ? $" (err={code.Value})"
                : (exitCode.HasValue ? $" (exit={exitCode.Value})" : string.Empty);

            return new TranslatedMountError
            {
                Title = "マウントに失敗しました" + codeSuffix,
                Message = string.IsNullOrWhiteSpace(eventMsg)
                    ? "zeroext エンジンがマウント中にエラーを返しました。"
                    : eventMsg,
                Suggestion = "「詳細 & ログ」タブで詳細な出力を確認してください。",
                RawDetail = allText,
            };
        }
    }
}
