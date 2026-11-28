using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using ZeroExt4.Gui.Models;

namespace ZeroExt4.Gui.Services
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

            if (allText.Contains("NTFS case sensitivity", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("FileCaseSensitiveInfo", StringComparison.OrdinalIgnoreCase))
            {
                if (ContainsWinErr(allText, 145) ||
                    code == -41 /* -ENOTEMPTY */)
                {
                    return new TranslatedMountError
                    {
                        Title = LocalizationService.Get("Str_ErrUpperCsNotEmptyTitle"),
                        Message = LocalizationService.Get("Str_ErrUpperCsNotEmptyMsg"),
                        Suggestion = LocalizationService.Get("Str_ErrUpperCsNotEmptySug"),
                        RawDetail = allText,
                    };
                }
                if (ContainsWinErr(allText, 5) ||
                    code == -13 /* -EACCES */)
                {
                    return new TranslatedMountError
                    {
                        Title = LocalizationService.Get("Str_ErrUpperCsAccessTitle"),
                        Message = LocalizationService.Get("Str_ErrUpperCsAccessMsg"),
                        Suggestion = LocalizationService.Get("Str_ErrUpperCsAccessSug"),
                        RawDetail = allText,
                    };
                }
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrUpperCsUnsupportedTitle"),
                    Message = LocalizationService.Get("Str_ErrUpperCsUnsupportedMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrUpperCsUnsupportedSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Unsupported .wh.__ext4_lower_id marker format version", StringComparison.OrdinalIgnoreCase) ||
                (code == -95 /* -ENOTSUP */ && allText.Contains(".wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase)))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrOverlayUnsupportedVerTitle"),
                    Message = LocalizationService.Get("Str_ErrOverlayUnsupportedVerMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrOverlayUnsupportedVerSug"),
                    RawDetail = allText,
                };
            }

            if ((allText.Contains("Corrupted", StringComparison.OrdinalIgnoreCase) ||
                 allText.Contains("Invalid .wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase) ||
                 allText.Contains("oversized .wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase) ||
                 allText.Contains("Failed to read .wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase)) &&
                allText.Contains(".wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrOverlayCorruptTitle"),
                    Message = LocalizationService.Get("Str_ErrOverlayCorruptMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrOverlayCorruptSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Refusing non-empty upperdir without .wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase) ||
                (code == -41 /* -ENOTEMPTY */ && allText.Contains("upperdir", StringComparison.OrdinalIgnoreCase)))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrOverlayLegacyRefusedTitle"),
                    Message = LocalizationService.Get("Str_ErrOverlayLegacyRefusedMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrOverlayLegacyRefusedSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("belongs to a different lower ext4 filesystem", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains(".wh.__ext4_lower_id", StringComparison.OrdinalIgnoreCase))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrOverlayMismatchTitle"),
                    Message = LocalizationService.Get("Str_ErrOverlayMismatchMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrOverlayMismatchSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Refusing RW mount on dirty ext4", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("without --force-dirty", StringComparison.OrdinalIgnoreCase) ||
                code == -30 /* -EROFS */)
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrDirtyRwTitle"),
                    Message = LocalizationService.Get("Str_ErrDirtyRwMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrDirtyRwSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("WinFsp runtime", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("winfsp-x64.dll", StringComparison.OrdinalIgnoreCase) ||
                code == -19 /* -ENODEV */)
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrWinFspTitle"),
                    Message = LocalizationService.Get("Str_ErrWinFspMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrWinFspSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("0xC0000022", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("0xC0000035", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("SetMountPoint failed", StringComparison.OrdinalIgnoreCase))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrMountPointTitle"),
                    Message = LocalizationService.Get("Str_ErrMountPointMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrMountPointSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Access Denied", StringComparison.OrdinalIgnoreCase) ||
                allText.Contains("アクセスが拒否", StringComparison.OrdinalIgnoreCase) ||
                code == -13 /* -EACCES */)
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrAccessDeniedTitle"),
                    Message = LocalizationService.Get("Str_ErrAccessDeniedMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrAccessDeniedSug"),
                    RawDetail = allText,
                };
            }

            if (allText.Contains("Failed to mount", StringComparison.OrdinalIgnoreCase) &&
                (code == -2 /* -ENOENT */ || code == -22 /* -EINVAL */))
            {
                return new TranslatedMountError
                {
                    Title = LocalizationService.Get("Str_ErrOpenFsTitle"),
                    Message = LocalizationService.Get("Str_ErrOpenFsMsg"),
                    Suggestion = LocalizationService.Get("Str_ErrOpenFsSug"),
                    RawDetail = allText,
                };
            }

            string codeSuffix = code.HasValue
                ? $" (err={code.Value})"
                : (exitCode.HasValue ? $" (exit={exitCode.Value})" : string.Empty);

            return new TranslatedMountError
            {
                Title = LocalizationService.Get("Str_ErrGenericMountTitle") + codeSuffix,
                Message = string.IsNullOrWhiteSpace(eventMsg)
                    ? LocalizationService.Get("Str_ErrGenericMountMsg")
                    : eventMsg,
                Suggestion = LocalizationService.Get("Str_ErrGenericMountSug"),
                RawDetail = allText,
            };
        }

        private static bool ContainsWinErr(string text, int winErr)
        {
            return !string.IsNullOrEmpty(text) &&
                   Regex.IsMatch(text, $@"\bwinerr={winErr}(?!\d)", RegexOptions.IgnoreCase);
        }
    }
}
