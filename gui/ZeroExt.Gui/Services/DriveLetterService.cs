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
                return (false, LocalizationService.Get("Str_FolderValEmpty"));
            }

            string trimmed = path.Trim();
            try
            {
                string full = Path.GetFullPath(trimmed);
                string? root = Path.GetPathRoot(full);
                if (string.Equals(full.TrimEnd('\\'), root?.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                {
                    return (false, LocalizationService.Get("Str_FolderValDriveRoot"));
                }
                if (File.Exists(full))
                {
                    return (false, LocalizationService.Get("Str_FolderValIsFile"));
                }
                if (Directory.Exists(full))
                {
                    if (Directory.EnumerateFileSystemEntries(full).Any())
                    {
                        return (false, LocalizationService.Get("Str_FolderValNotEmpty"));
                    }
                    return (true, LocalizationService.Get("Str_FolderValOk"));
                }

                string? parent = Path.GetDirectoryName(full);
                if (string.IsNullOrEmpty(parent) || !Directory.Exists(parent))
                {
                    return (false, LocalizationService.Get("Str_FolderValNoParent"));
                }
                return (false, LocalizationService.Get("Str_FolderValNotExist"));
            }
            catch (Exception ex)
            {
                return (false, LocalizationService.Format("Str_FolderValInvalidFmt", ex.Message));
            }
        }

        public static UpperDirCheckResult CheckOverlayUpperDir(string? upperDir, string? expectedLowerId)
        {
            if (string.IsNullOrWhiteSpace(upperDir))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.None,
                    Message = LocalizationService.Get("Str_UpperValNone"),
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
                    Message = LocalizationService.Format("Str_UpperValInvalidFmt", ex.Message),
                    CanMount = false,
                };
            }

            string? root = Path.GetPathRoot(full);
            if (string.Equals(full.TrimEnd('\\'), root?.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.InvalidPath,
                    Message = LocalizationService.Get("Str_UpperValDriveRoot"),
                    CanMount = false,
                };
            }

            if (File.Exists(full))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.InvalidPath,
                    Message = LocalizationService.Get("Str_UpperValIsFile"),
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
                        Message = LocalizationService.Format("Str_UpperValNoDriveFmt", root),
                        CanMount = false,
                    };
                }
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.WillCreate,
                    Message = LocalizationService.Get("Str_UpperValWillCreate"),
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
                        Message = LocalizationService.Get("Str_UpperValMismatch"),
                        CanMount = false,
                    };
                }

                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.MatchingExisting,
                    Message = LocalizationService.Get("Str_UpperValMatch"),
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
                        Message = LocalizationService.Get("Str_UpperValNonEmptyUnmarked"),
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
                Message = LocalizationService.Get("Str_UpperValEmptyReady"),
                CanMount = true,
            };
        }
    }
}
