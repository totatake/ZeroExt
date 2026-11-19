using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using ZeroExt.Gui.Models;

namespace ZeroExt.Gui.Services
{
    public static class DriveLetterService
    {
        private const string LowerIdFileName = ".wh.__ext4_lower_id";

        [DllImport("kernel32.dll")]
        private static extern uint GetLogicalDrives();

        public static List<string> GetAvailableDriveLetters(IEnumerable<string>? additionalUsed = null)
        {
            var used = new HashSet<char>();

            try
            {
                uint mask = GetLogicalDrives();
                for (int i = 0; i < 26; i++)
                {
                    if ((mask & (1U << i)) != 0)
                    {
                        used.Add((char)('A' + i));
                    }
                }
            }
            catch
            {
                // Fallback below
            }

            try
            {
                foreach (DriveInfo d in DriveInfo.GetDrives())
                {
                    if (!string.IsNullOrEmpty(d.Name))
                    {
                        used.Add(char.ToUpperInvariant(d.Name[0]));
                    }
                }
            }
            catch
            {
                // Ignore enumeration error
            }

            if (additionalUsed != null)
            {
                foreach (string item in additionalUsed)
                {
                    if (!string.IsNullOrWhiteSpace(item) && char.IsLetter(item.Trim()[0]))
                    {
                        used.Add(char.ToUpperInvariant(item.Trim()[0]));
                    }
                }
            }

            // Prefer M:..Z: first (matching ZeroExt docs), then E:..L:, then D:
            var ordered = new List<char>();
            for (char c = 'M'; c <= 'Z'; c++) ordered.Add(c);
            for (char c = 'E'; c <= 'L'; c++) ordered.Add(c);
            ordered.Add('D');

            var available = new List<string>();
            foreach (char c in ordered)
            {
                if (!used.Contains(c))
                {
                    available.Add($"{c}:");
                }
            }
            return available;
        }

        public static bool IsDriveLetter(string? value)
        {
            if (string.IsNullOrWhiteSpace(value)) return false;
            string s = value.Trim();
            return (s.Length == 2 && char.IsLetter(s[0]) && s[1] == ':') ||
                   (s.Length == 3 && char.IsLetter(s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'));
        }

        public static (bool IsValid, string Message) ValidateMountFolder(string? path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return (false, "マウント先フォルダのパスを入力してください。");
            }

            string trimmed = path.Trim();
            try
            {
                string full = Path.GetFullPath(trimmed);
                string? root = Path.GetPathRoot(full);
                if (string.Equals(full.TrimEnd('\\'), root?.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                {
                    return (false, "ドライブルート直下（例: C:\\）はマウント先に指定できません。");
                }
                if (File.Exists(full))
                {
                    return (false, "指定されたパスは既存のファイルです。空のフォルダを指定してください。");
                }
                if (Directory.Exists(full))
                {
                    if (Directory.EnumerateFileSystemEntries(full).Any())
                    {
                        return (false, "指定されたフォルダは空ではありません。空のフォルダを指定してください。");
                    }
                    return (true, "空のフォルダを確認しました。ここにマウントできます。");
                }

                string? parent = Path.GetDirectoryName(full);
                if (string.IsNullOrEmpty(parent) || !Directory.Exists(parent))
                {
                    return (false, "親フォルダが存在しません。既存の NTFS ドライブ配下のパスを指定してください。");
                }
                return (false, "指定されたフォルダが存在しません。マウント前に空のフォルダを作成してください。");
            }
            catch (Exception ex)
            {
                return (false, $"無効なフォルダパスです: {ex.Message}");
            }
        }

        public static UpperDirCheckResult CheckOverlayUpperDir(string? upperDir, string? expectedLowerId)
        {
            if (string.IsNullOrWhiteSpace(upperDir))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.None,
                    Message = "Overlay の書き込み先となる Windows (NTFS) フォルダを指定してください。",
                    CanMount = false,
                };
            }

            string full;
            try
            {
                full = Path.GetFullPath(upperDir.Trim());
            }
            catch (Exception ex)
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.InvalidPath,
                    Message = $"無効なパスです: {ex.Message}",
                    CanMount = false,
                };
            }

            string? root = Path.GetPathRoot(full);
            if (string.Equals(full.TrimEnd('\\'), root?.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.InvalidPath,
                    Message = "ドライブのルート（例: D:\\）は Upperdir に指定できません。サブフォルダを指定してください。",
                    CanMount = false,
                };
            }

            if (File.Exists(full))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.InvalidPath,
                    Message = "指定されたパスはファイルです。フォルダを指定してください。",
                    CanMount = false,
                };
            }

            if (!Directory.Exists(full))
            {
                if (string.IsNullOrEmpty(root) || !Directory.Exists(root))
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.InvalidPath,
                        Message = $"ドライブ '{root}' が見つかりません。",
                        CanMount = false,
                    };
                }
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.WillCreate,
                    Message = "フォルダが存在しないため、マウント時に新規作成され Lower ID が記録されます。",
                    CanMount = true,
                };
            }

            string markerPath = Path.Combine(full, LowerIdFileName);
            if (File.Exists(markerPath))
            {
                string stored = string.Empty;
                try
                {
                    stored = File.ReadAllText(markerPath).Trim();
                }
                catch
                {
                    // Ignore read error and let engine check
                }

                if (!string.IsNullOrWhiteSpace(expectedLowerId) &&
                    !string.Equals(stored, expectedLowerId, StringComparison.Ordinal))
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.MismatchedLower,
                        Message = $"このフォルダは別の ext4 ボリューム用の Upperdir です（.wh.__ext4_lower_id 不一致）。別の空フォルダを指定してください。",
                        CanMount = false,
                    };
                }

                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.MatchingExisting,
                    Message = "同一 ext4 ボリュームに紐付いた既存の Upperdir です（前回の変更を継続できます）。",
                    CanMount = true,
                };
            }

            try
            {
                if (Directory.EnumerateFileSystemEntries(full).Any())
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.NonEmptyUnmarked,
                        Message = "警告: フォルダが空ではなく Lower ID マーカーがありません。既存ファイルが Lower 側を上書き表示します。",
                        CanMount = true,
                    };
                }
            }
            catch
            {
                // Ignore enumeration error
            }

            return new UpperDirCheckResult
            {
                Status = UpperDirCheckStatus.EmptyReady,
                Message = "空のフォルダを確認しました。Lower ext4 は完全に保護されたまま利用できます。",
                CanMount = true,
            };
        }
    }
}
