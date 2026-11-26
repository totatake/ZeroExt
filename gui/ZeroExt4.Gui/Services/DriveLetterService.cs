using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using ZeroExt4.Gui.Models;

namespace ZeroExt4.Gui.Services
{
    public static class DriveLetterService
    {
        private const string LowerIdFileName = ".wh.__ext4_lower_id";

        [DllImport("kernel32.dll")]
        private static extern uint GetLogicalDrives();

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint QueryDosDeviceW(string lpDeviceName, [Out] char[] lpTargetPath, uint ucchMax);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern uint GetDriveTypeW(string lpRootPathName);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFileW(
            string lpFileName,
            uint dwDesiredAccess,
            uint dwShareMode,
            IntPtr lpSecurityAttributes,
            uint dwCreationDisposition,
            uint dwFlagsAndAttributes,
            IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandleEx(
            IntPtr hFile,
            int FileInformationClass,
            out uint lpFileInformation,
            uint dwBufferSize);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);

        public static bool IsDirectoryCaseSensitive(string dirPath)
        {
            const uint FILE_READ_ATTRIBUTES = 0x0080;
            const uint FILE_SHARE_ALL = 0x00000001 | 0x00000002 | 0x00000004;
            const uint OPEN_EXISTING = 3;
            const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
            const int FileCaseSensitiveInfo = 23;
            const uint FILE_CS_FLAG_CASE_SENSITIVE_DIR = 1;

            if (string.IsNullOrWhiteSpace(dirPath)) return false;
            IntPtr h = CreateFileW(dirPath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, IntPtr.Zero,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, IntPtr.Zero);
            if (h == new IntPtr(-1)) return false;
            try
            {
                if (GetFileInformationByHandleEx(h, FileCaseSensitiveInfo, out uint flags, sizeof(uint)))
                {
                    return (flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0;
                }
                return false;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetVolumeInformationW(
            string lpRootPathName,
            [Out] char[]? lpVolumeNameBuffer,
            uint nVolumeNameSize,
            out uint lpVolumeSerialNumber,
            out uint lpMaximumComponentLength,
            out uint lpFileSystemFlags,
            [Out] char[] lpFileSystemNameBuffer,
            uint nFileSystemNameSize);

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

            // Prefer L:..Z: first (matching ZeroExt4 docs), then E:..K:, then D:
            var ordered = new List<char>();
            for (char c = 'L'; c <= 'Z'; c++) ordered.Add(c);
            for (char c = 'E'; c <= 'K'; c++) ordered.Add(c);
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

        public static UpperDirCheckResult CheckOverlayUpperDir(string? upperDir, string? expectedLowerId, bool caseSensitive = false)
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

                string effectiveExpected = (!string.IsNullOrWhiteSpace(expectedLowerId) && caseSensitive)
                    ? expectedLowerId + ":cs"
                    : (expectedLowerId ?? string.Empty);

                if (!string.IsNullOrWhiteSpace(effectiveExpected) &&
                    !string.Equals(stored, effectiveExpected, StringComparison.Ordinal))
                {
                    bool modeMismatchOnly = !string.IsNullOrWhiteSpace(expectedLowerId) &&
                        (string.Equals(stored, expectedLowerId, StringComparison.Ordinal) ||
                         string.Equals(stored, expectedLowerId + ":cs", StringComparison.Ordinal));
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.MismatchedLower,
                        Message = LocalizationService.Get(modeMismatchOnly
                            ? "Str_UpperValCsModeMismatch"
                            : "Str_UpperValMismatch"),
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
                    if (caseSensitive && !IsDirectoryCaseSensitive(full))
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.InvalidPath,
                            Message = LocalizationService.Get("Str_UpperValCsNonEmpty"),
                            CanMount = false,
                        };
                    }
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

        public static bool IsSafeUnmountTarget(string? dosTarget, bool volumeAlive, string? fsName)
        {
            if (string.IsNullOrWhiteSpace(dosTarget)) return false;
            if (!dosTarget.StartsWith(@"\Device\Volume{", StringComparison.OrdinalIgnoreCase) &&
                !dosTarget.StartsWith(@"\Device\WinFsp.", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            if (volumeAlive)
            {
                return string.Equals(fsName, "ext4", StringComparison.OrdinalIgnoreCase);
            }
            return true;
        }

        public static DriveUnmountInspection InspectDriveForUnmount(string? driveLetter)
        {
            string normalized = ZeroExt4ArgBuilder.NormalizeMountPointArg(driveLetter ?? string.Empty).ToUpperInvariant();
            if (!IsDriveLetter(normalized))
            {
                return new DriveUnmountInspection { DriveLetter = normalized };
            }

            string devName = normalized.Substring(0, 2);
            string root = devName + "\\";
            var targetBuf = new char[512];
            uint rc = QueryDosDeviceW(devName, targetBuf, (uint)targetBuf.Length);
            string dosTarget;
            if (rc != 0)
            {
                int nulIdx = Array.IndexOf(targetBuf, '\0');
                dosTarget = nulIdx >= 0 ? new string(targetBuf, 0, nulIdx) : new string(targetBuf);
            }
            else if (GetDriveTypeW(root) != 1 /* DRIVE_NO_ROOT_DIR */)
            {
                dosTarget = "<unknown>";
            }
            else
            {
                return new DriveUnmountInspection { DriveLetter = devName };
            }

            var fsBuf = new char[64];
            bool volumeAlive = false;
            string fsName = string.Empty;
            if (GetVolumeInformationW(root, null, 0, out _, out _, out _, fsBuf, (uint)fsBuf.Length))
            {
                volumeAlive = true;
                int fsNul = Array.IndexOf(fsBuf, '\0');
                fsName = fsNul >= 0 ? new string(fsBuf, 0, fsNul) : new string(fsBuf);
            }
            else
            {
                int err = Marshal.GetLastWin32Error();
                if (err == 5 /* ERROR_ACCESS_DENIED */ || err == 32 /* ERROR_SHARING_VIOLATION */)
                {
                    volumeAlive = true;
                }
            }

            bool safe = IsSafeUnmountTarget(dosTarget, volumeAlive, fsName);
            return new DriveUnmountInspection
            {
                DriveLetter = devName,
                HasTarget = true,
                DosTarget = dosTarget,
                VolumeAlive = volumeAlive,
                FileSystemName = fsName,
                IsSafeToUnmount = safe,
                IsStale = safe && !volumeAlive,
            };
        }

        public static List<DriveUnmountInspection> FindSafeUnmountCandidates()
        {
            var list = new List<DriveUnmountInspection>();
            for (char c = 'D'; c <= 'Z'; c++)
            {
                DriveUnmountInspection info = InspectDriveForUnmount($"{c}:");
                if (info.HasTarget && info.IsSafeToUnmount)
                {
                    list.Add(info);
                }
            }
            return list;
        }
    }

    public sealed class DriveUnmountInspection
    {
        public string DriveLetter { get; init; } = string.Empty;
        public bool HasTarget { get; init; }
        public string DosTarget { get; init; } = string.Empty;
        public bool VolumeAlive { get; init; }
        public string FileSystemName { get; init; } = string.Empty;
        public bool IsSafeToUnmount { get; init; }
        public bool IsStale { get; init; }
    }
}
