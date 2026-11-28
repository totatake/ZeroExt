using System;
using System.Collections.Generic;
using System.Globalization;
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

        public static UpperDirCheckResult CheckOverlayUpperDir(string? upperDir, PartitionInfo? partition, bool caseSensitive = false)
        {
            return CheckOverlayUpperDirCore(upperDir, partition?.ExpectedOverlayLowerId, partition, caseSensitive);
        }

        public static UpperDirCheckResult CheckOverlayUpperDir(string? upperDir, string? expectedLowerId, bool caseSensitive = false)
        {
            return CheckOverlayUpperDirCore(upperDir, expectedLowerId, null, caseSensitive);
        }

        private static UpperDirCheckResult CheckOverlayUpperDirCore(
            string? upperDir,
            string? expectedLowerId,
            PartitionInfo? partition,
            bool caseSensitive)
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
                string stored;
                try
                {
                    var fi = new FileInfo(markerPath);
                    if (fi.Length <= 0 || fi.Length > 510)
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.CorruptMarker,
                            Message = LocalizationService.Get("Str_UpperValCorrupt"),
                            CanMount = false,
                        };
                    }
                    stored = File.ReadAllText(markerPath).TrimEnd('\r', '\n');
                }
                catch
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.CorruptMarker,
                        Message = LocalizationService.Get("Str_UpperValCorrupt"),
                        CanMount = false,
                    };
                }

                if (string.IsNullOrEmpty(stored) || stored.Any(c => c < 0x20 || c == 0x7F))
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.CorruptMarker,
                        Message = LocalizationService.Get("Str_UpperValCorrupt"),
                        CanMount = false,
                    };
                }

                return ValidateMarkerContent(stored, expectedLowerId, partition, caseSensitive);
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
                        CanMount = false,
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

        private static UpperDirCheckResult ValidateMarkerContent(
            string stored,
            string? expectedLowerId,
            PartitionInfo? partition,
            bool caseSensitive)
        {
            if (stored.StartsWith("v2;", StringComparison.Ordinal))
            {
                var map = new Dictionary<string, string>(StringComparer.Ordinal);
                string[] tokens = stored.Substring(3).Split(';');
                foreach (string tok in tokens)
                {
                    if (string.IsNullOrEmpty(tok))
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.CorruptMarker,
                            Message = LocalizationService.Get("Str_UpperValCorrupt"),
                            CanMount = false,
                        };
                    }
                    int eq = tok.IndexOf('=');
                    if (eq <= 0 || !map.TryAdd(tok.Substring(0, eq), tok.Substring(eq + 1)))
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.CorruptMarker,
                            Message = LocalizationService.Get("Str_UpperValCorrupt"),
                            CanMount = false,
                        };
                    }
                }

                if (map.Count != 7 ||
                    !map.TryGetValue("uuid", out string? mUuid) || string.IsNullOrWhiteSpace(mUuid) ||
                    !map.TryGetValue("blocks", out string? mBlocksStr) || !ulong.TryParse(mBlocksStr, NumberStyles.None, CultureInfo.InvariantCulture, out ulong mBlocks) ||
                    !map.TryGetValue("bsize", out string? mBsizeStr) || !uint.TryParse(mBsizeStr, NumberStyles.None, CultureInfo.InvariantCulture, out uint mBsize) || mBsize < 1024 ||
                    !map.TryGetValue("part_uuid", out string? mPartUuid) ||
                    !map.TryGetValue("part_off", out string? mOffStr) || !ulong.TryParse(mOffStr, NumberStyles.None, CultureInfo.InvariantCulture, out ulong mPartOff) ||
                    !map.TryGetValue("part_size", out string? mSizeStr) || !ulong.TryParse(mSizeStr, NumberStyles.None, CultureInfo.InvariantCulture, out ulong mPartSize) ||
                    !map.TryGetValue("cs", out string? mCsStr) || (mCsStr != "0" && mCsStr != "1"))
                {
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.CorruptMarker,
                        Message = LocalizationService.Get("Str_UpperValCorrupt"),
                        CanMount = false,
                    };
                }

                bool mCs = mCsStr == "1";
                if (partition != null && !string.IsNullOrWhiteSpace(partition.Uuid))
                {
                    ulong expSize = partition.SizeBytes > 0 ? partition.SizeBytes : partition.EffectiveTotalBytes;
                    bool idMatch = string.Equals(mUuid, partition.Uuid, StringComparison.OrdinalIgnoreCase) &&
                                   mBlocks == partition.BlocksCount &&
                                   mBsize == partition.BlockSize &&
                                   string.Equals(mPartUuid, partition.PartitionUuid ?? string.Empty, StringComparison.OrdinalIgnoreCase) &&
                                   mPartOff == partition.OffsetBytes &&
                                   mPartSize == expSize;
                    if (!idMatch)
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.MismatchedLower,
                            Message = LocalizationService.Get("Str_UpperValMismatch"),
                            CanMount = false,
                        };
                    }
                    if (mCs != caseSensitive)
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.MismatchedLower,
                            Message = LocalizationService.Get("Str_UpperValCsModeMismatch"),
                            CanMount = false,
                        };
                    }
                }
                else if (!string.IsNullOrWhiteSpace(expectedLowerId))
                {
                    string v1Equivalent = mCs ? $"{mUuid}:{mBlocks}:{mBsize}:cs" : $"{mUuid}:{mBlocks}:{mBsize}";
                    string effectiveExpected = caseSensitive ? expectedLowerId + ":cs" : expectedLowerId;
                    if (!string.Equals(v1Equivalent, effectiveExpected, StringComparison.OrdinalIgnoreCase))
                    {
                        bool modeMismatchOnly =
                            string.Equals(v1Equivalent, expectedLowerId, StringComparison.OrdinalIgnoreCase) ||
                            string.Equals(v1Equivalent, expectedLowerId + ":cs", StringComparison.OrdinalIgnoreCase);
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.MismatchedLower,
                            Message = LocalizationService.Get(modeMismatchOnly
                                ? "Str_UpperValCsModeMismatch"
                                : "Str_UpperValMismatch"),
                            CanMount = false,
                        };
                    }
                }

                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.MatchingExisting,
                    Message = LocalizationService.Get("Str_UpperValMatch"),
                    CanMount = true,
                };
            }

            if (stored.Length >= 3 && stored[0] == 'v' && char.IsDigit(stored[1]))
            {
                int semiIdx = stored.IndexOf(';');
                if (semiIdx > 1 &&
                    ulong.TryParse(stored.AsSpan(1, semiIdx - 1), NumberStyles.None, CultureInfo.InvariantCulture, out ulong ver))
                {
                    if (ver > 2)
                    {
                        return new UpperDirCheckResult
                        {
                            Status = UpperDirCheckStatus.UnsupportedMarkerVersion,
                            Message = LocalizationService.Format("Str_UpperValUnsupportedVerFmt", ver),
                            CanMount = false,
                        };
                    }
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.CorruptMarker,
                        Message = LocalizationService.Get("Str_UpperValCorrupt"),
                        CanMount = false,
                    };
                }
            }

            // Legacy v1 format: <uuid>:<blocks>:<bsize>[:cs]
            string[] parts = stored.Split(':');
            if ((parts.Length != 3 && parts.Length != 4) ||
                string.IsNullOrWhiteSpace(parts[0]) ||
                !ulong.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out _) ||
                !uint.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out uint v1Bsize) ||
                v1Bsize < 1024 ||
                (parts.Length == 4 && parts[3] != "cs"))
            {
                return new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.CorruptMarker,
                    Message = LocalizationService.Get("Str_UpperValCorrupt"),
                    CanMount = false,
                };
            }

            string? expectedV1 = !string.IsNullOrWhiteSpace(expectedLowerId)
                ? expectedLowerId
                : partition?.ExpectedOverlayLowerId;
            if (!string.IsNullOrWhiteSpace(expectedV1))
            {
                string effectiveExpected = caseSensitive ? expectedV1 + ":cs" : expectedV1;
                if (!string.Equals(stored, effectiveExpected, StringComparison.OrdinalIgnoreCase))
                {
                    bool modeMismatchOnly =
                        string.Equals(stored, expectedV1, StringComparison.OrdinalIgnoreCase) ||
                        string.Equals(stored, expectedV1 + ":cs", StringComparison.OrdinalIgnoreCase);
                    return new UpperDirCheckResult
                    {
                        Status = UpperDirCheckStatus.MismatchedLower,
                        Message = LocalizationService.Get(modeMismatchOnly
                            ? "Str_UpperValCsModeMismatch"
                            : "Str_UpperValMismatch"),
                        CanMount = false,
                    };
                }
            }

            return new UpperDirCheckResult
            {
                Status = UpperDirCheckStatus.MatchingExisting,
                Message = LocalizationService.Get("Str_UpperValMatch"),
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
