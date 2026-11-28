using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.Json.Serialization;
using ZeroExt4.Gui.Services;

namespace ZeroExt4.Gui.Models
{
    public sealed class ScanResponse
    {
        [JsonPropertyName("is_elevated")]
        public bool IsElevated { get; set; }

        [JsonPropertyName("winfsp_available")]
        public bool WinFspAvailable { get; set; }

        [JsonPropertyName("disks")]
        public List<DiskInfo> Disks { get; set; } = new List<DiskInfo>();
    }

    public sealed class DiskInfo
    {
        [JsonPropertyName("device_path")]
        public string DevicePath { get; set; } = string.Empty;

        [JsonPropertyName("drive_index")]
        public int DriveIndex { get; set; }

        [JsonPropertyName("accessible")]
        public bool Accessible { get; set; }

        [JsonPropertyName("style")]
        public string Style { get; set; } = string.Empty;

        [JsonPropertyName("effective_sector_size")]
        public uint EffectiveSectorSize { get; set; }

        [JsonPropertyName("total_size_bytes")]
        public ulong TotalSizeBytes { get; set; }

        [JsonPropertyName("partitions")]
        public List<PartitionInfo> Partitions { get; set; } = new List<PartitionInfo>();

        [JsonIgnore]
        public bool IsImageFile => DriveIndex < 0;

        [JsonIgnore]
        public string DisplayTitle =>
            IsImageFile
                ? Path.GetFileName(DevicePath)
                : LocalizationService.Format("Str_DiskTitleFmt", DriveIndex);

        [JsonIgnore]
        public string SubtitleText =>
            IsImageFile
                ? DevicePath
                : Accessible
                    ? $"{DevicePath}  |  {Style}  |  {ByteFormatter.Format(TotalSizeBytes)}"
                    : LocalizationService.Format("Str_DiskNeedsElevation", DevicePath);
    }

    public sealed class PartitionInfo
    {
        [JsonPropertyName("partition_number")]
        public uint PartitionNumber { get; set; }

        [JsonPropertyName("start_sector")]
        public ulong StartSector { get; set; }

        [JsonPropertyName("sector_count")]
        public ulong SectorCount { get; set; }

        [JsonPropertyName("offset_bytes")]
        public ulong OffsetBytes { get; set; }

        [JsonPropertyName("size_bytes")]
        public ulong SizeBytes { get; set; }

        [JsonPropertyName("type_id")]
        public string TypeId { get; set; } = string.Empty;

        [JsonPropertyName("type_description")]
        public string TypeDescription { get; set; } = string.Empty;

        [JsonPropertyName("partition_name")]
        public string PartitionName { get; set; } = string.Empty;

        [JsonPropertyName("partition_uuid")]
        public string PartitionUuid { get; set; } = string.Empty;

        [JsonPropertyName("fs_kind")]
        public string FsKind { get; set; } = string.Empty;

        [JsonPropertyName("journal_state")]
        public string JournalState { get; set; } = string.Empty;

        [JsonPropertyName("volume_label")]
        public string VolumeLabel { get; set; } = string.Empty;

        [JsonPropertyName("uuid")]
        public string Uuid { get; set; } = string.Empty;

        [JsonPropertyName("block_size")]
        public uint BlockSize { get; set; }

        [JsonPropertyName("inode_size")]
        public uint InodeSize { get; set; }

        [JsonPropertyName("blocks_count")]
        public ulong BlocksCount { get; set; }

        [JsonPropertyName("total_bytes")]
        public ulong TotalBytes { get; set; }

        [JsonPropertyName("free_bytes")]
        public ulong FreeBytes { get; set; }

        [JsonPropertyName("feature_compat")]
        public uint FeatureCompat { get; set; }

        [JsonPropertyName("feature_incompat")]
        public uint FeatureIncompat { get; set; }

        [JsonPropertyName("feature_ro_compat")]
        public uint FeatureRoCompat { get; set; }

        [JsonPropertyName("journal_inum")]
        public uint JournalInum { get; set; }

        [JsonPropertyName("journal_start")]
        public uint JournalStart { get; set; }

        [JsonPropertyName("journal_errno")]
        public int JournalErrno { get; set; }

        [JsonPropertyName("state_flags")]
        public uint StateFlags { get; set; }

        [JsonPropertyName("feature_compat_names")]
        public List<string> FeatureCompatNames { get; set; } = new List<string>();

        [JsonPropertyName("feature_incompat_names")]
        public List<string> FeatureIncompatNames { get; set; } = new List<string>();

        [JsonPropertyName("feature_ro_compat_names")]
        public List<string> FeatureRoCompatNames { get; set; } = new List<string>();

        [JsonPropertyName("last_mount_time")]
        public ulong LastMountTime { get; set; }

        [JsonPropertyName("last_fsck_time")]
        public ulong LastFsckTime { get; set; }

        [JsonPropertyName("fsck_error_count")]
        public uint FsckErrorCount { get; set; }

        [JsonPropertyName("metadata_csum_enabled")]
        public bool MetadataCsumEnabled { get; set; }

        [JsonPropertyName("sb_checksum_valid")]
        public bool SbChecksumValid { get; set; }

        [JsonPropertyName("mount_advice")]
        public MountAdviceInfo? MountAdvice { get; set; }

        [JsonIgnore]
        public bool IsExtFs =>
            string.Equals(FsKind, "ext2", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(FsKind, "ext3", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(FsKind, "ext4", StringComparison.OrdinalIgnoreCase);

        [JsonIgnore]
        public bool IsDirty =>
            string.Equals(JournalState, "NEEDS_RECOVERY", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(JournalState, "HAS_ERRORS", StringComparison.OrdinalIgnoreCase);

        [JsonIgnore]
        public string? ExpectedOverlayLowerId =>
            (!string.IsNullOrWhiteSpace(Uuid) && BlockSize > 0 && BlocksCount > 0)
                ? $"{Uuid}:{BlocksCount}:{BlockSize}"
                : null;

        public string? GetExpectedOverlayLowerIdV2(bool caseSensitive = false)
        {
            if (string.IsNullOrWhiteSpace(Uuid) || BlockSize == 0 || BlocksCount == 0)
            {
                return null;
            }
            ulong partSize = SizeBytes > 0 ? SizeBytes : EffectiveTotalBytes;
            int cs = caseSensitive ? 1 : 0;
            return string.Create(
                CultureInfo.InvariantCulture,
                $"v2;uuid={Uuid};blocks={BlocksCount};bsize={BlockSize};part_uuid={PartitionUuid};part_off={OffsetBytes};part_size={partSize};cs={cs}");
        }

        [JsonIgnore]
        public string FeaturesSummary
        {
            get
            {
                var all = new List<string>();
                if (FeatureCompatNames != null) all.AddRange(FeatureCompatNames);
                if (FeatureIncompatNames != null) all.AddRange(FeatureIncompatNames);
                if (FeatureRoCompatNames != null) all.AddRange(FeatureRoCompatNames);
                return all.Count > 0 ? string.Join(", ", all) : "-";
            }
        }

        [JsonIgnore]
        public string ChecksumSummary =>
            !IsExtFs
                ? "-"
                : (!MetadataCsumEnabled
                    ? "disabled"
                    : (SbChecksumValid ? "CRC32C OK" : "CHECKSUM MISMATCH"));

        [JsonIgnore]
        public ulong EffectiveTotalBytes => TotalBytes > 0 ? TotalBytes : SizeBytes;

        [JsonIgnore]
        public ulong UsedBytes =>
            EffectiveTotalBytes > FreeBytes ? EffectiveTotalBytes - FreeBytes : 0UL;

        [JsonIgnore]
        public double UsedPercent =>
            EffectiveTotalBytes == 0
                ? 0.0
                : Math.Clamp((double)UsedBytes * 100.0 / (double)EffectiveTotalBytes, 0.0, 100.0);

        [JsonIgnore]
        public string DisplayName
        {
            get
            {
                if (!string.IsNullOrWhiteSpace(VolumeLabel))
                {
                    return VolumeLabel;
                }
                if (!string.IsNullOrWhiteSpace(PartitionName))
                {
                    return PartitionName;
                }
                return PartitionNumber == 0
                    ? LocalizationService.Get("Str_RawImage")
                    : LocalizationService.Format("Str_PartitionNumFmt", PartitionNumber);
            }
        }

        [JsonIgnore]
        public string JournalDisplayText => JournalState switch
        {
            "CLEAN" => "CLEAN",
            "NONE" => LocalizationService.Get("Str_JournalNone"),
            "NEEDS_RECOVERY" => LocalizationService.Get("Str_JournalNeedsRecovery"),
            "HAS_ERRORS" => LocalizationService.Get("Str_JournalHasErrors"),
            _ => string.IsNullOrEmpty(JournalState) ? "-" : JournalState,
        };

        [JsonIgnore]
        public string JournalDescription => JournalState switch
        {
            "CLEAN" => LocalizationService.Get("Str_JournalDescClean"),
            "NONE" => LocalizationService.Get("Str_JournalDescNone"),
            "NEEDS_RECOVERY" => LocalizationService.Get("Str_JournalDescNeedsRecovery"),
            "HAS_ERRORS" => LocalizationService.Get("Str_JournalDescHasErrors"),
            _ => LocalizationService.Get("Str_JournalDescUnknown"),
        };
    }

    public sealed class LsResponse
    {
        [JsonPropertyName("source")]
        public string Source { get; set; } = string.Empty;

        [JsonPropertyName("partition")]
        public uint Partition { get; set; }

        [JsonPropertyName("path")]
        public string Path { get; set; } = "/";

        [JsonPropertyName("entries")]
        public List<LsEntry> Entries { get; set; } = new List<LsEntry>();
    }

    public sealed class LsEntry
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("is_dir")]
        public bool IsDir { get; set; }

        [JsonPropertyName("is_symlink")]
        public bool IsSymlink { get; set; }

        [JsonPropertyName("size")]
        public ulong Size { get; set; }

        [JsonPropertyName("mode")]
        public uint Mode { get; set; }

        [JsonPropertyName("uid")]
        public uint Uid { get; set; }

        [JsonPropertyName("gid")]
        public uint Gid { get; set; }

        [JsonPropertyName("ino")]
        public uint Inode { get; set; }

        [JsonPropertyName("hard_links")]
        public uint HardLinks { get; set; }

        [JsonPropertyName("last_write_filetime")]
        public ulong LastWriteFileTime { get; set; }

        [JsonIgnore]
        public string TypeLabel => IsDir
            ? LocalizationService.Get("Str_TypeDir")
            : (IsSymlink ? LocalizationService.Get("Str_TypeSymlink") : LocalizationService.Get("Str_TypeFile"));

        [JsonIgnore]
        public string SizeFormatted => IsDir ? "-" : ByteFormatter.Format(Size);

        [JsonIgnore]
        public string ModeOctal => Convert.ToString(Mode & 0xFFF, 8).PadLeft(4, '0');

        [JsonIgnore]
        public string OwnerFormatted => $"{Uid}:{Gid}";

        [JsonIgnore]
        public string ModifiedFormatted
        {
            get
            {
                if (LastWriteFileTime == 0 || LastWriteFileTime > long.MaxValue)
                {
                    return "-";
                }
                try
                {
                    DateTime dt = DateTime.FromFileTime((long)LastWriteFileTime);
                    return dt.ToString("yyyy/MM/dd HH:mm", CultureInfo.InvariantCulture);
                }
                catch
                {
                    return "-";
                }
            }
        }
    }

    public sealed class MountEventMessage
    {
        [JsonPropertyName("event")]
        public string Event { get; set; } = string.Empty;

        [JsonPropertyName("stage")]
        public string? Stage { get; set; }

        [JsonPropertyName("code")]
        public int? Code { get; set; }

        [JsonPropertyName("message")]
        public string? Message { get; set; }

        [JsonPropertyName("source")]
        public string? Source { get; set; }

        [JsonPropertyName("partition")]
        public uint? Partition { get; set; }

        [JsonPropertyName("mountpoint")]
        public string? MountPoint { get; set; }

        [JsonPropertyName("mode")]
        public string? Mode { get; set; }

        [JsonPropertyName("read_only")]
        public bool? ReadOnly { get; set; }

        [JsonPropertyName("lower_read_only")]
        public bool? LowerReadOnly { get; set; }

        [JsonPropertyName("direct_io")]
        public bool? DirectIo { get; set; }

        [JsonPropertyName("fell_back_to_ro")]
        public bool? FellBackToRo { get; set; }

        [JsonPropertyName("overlay_upper")]
        public string? OverlayUpper { get; set; }

        [JsonPropertyName("volume_label")]
        public string? VolumeLabel { get; set; }

        [JsonPropertyName("read_bytes")]
        public long? ReadBytes { get; set; }

        [JsonPropertyName("write_bytes")]
        public long? WriteBytes { get; set; }

        [JsonPropertyName("read_ops")]
        public long? ReadOps { get; set; }

        [JsonPropertyName("write_ops")]
        public long? WriteOps { get; set; }

        [JsonPropertyName("trace_path")]
        public string? TracePath { get; set; }

        [JsonPropertyName("trace_events")]
        public ulong? TraceEvents { get; set; }

        [JsonPropertyName("trace_dropped")]
        public ulong? TraceDropped { get; set; }
    }

    public sealed class MountAdviceInfo
    {
        [JsonPropertyName("read_only_allowed")]
        public bool ReadOnlyAllowed { get; set; }

        [JsonPropertyName("overlay_allowed")]
        public bool OverlayAllowed { get; set; }

        [JsonPropertyName("read_write_allowed")]
        public bool ReadWriteAllowed { get; set; }

        [JsonPropertyName("recommended_mode")]
        public string RecommendedMode { get; set; } = string.Empty;

        [JsonPropertyName("ro_reason")]
        public string RoReason { get; set; } = string.Empty;

        [JsonPropertyName("overlay_reason")]
        public string OverlayReason { get; set; } = string.Empty;

        [JsonPropertyName("rw_reason")]
        public string RwReason { get; set; } = string.Empty;
    }

    public sealed class DoctorResponse
    {
        [JsonPropertyName("overall_status")]
        public string OverallStatus { get; set; } = string.Empty;

        [JsonPropertyName("is_elevated")]
        public bool IsElevated { get; set; }

        [JsonPropertyName("physical_disk_access")]
        public bool PhysicalDiskAccess { get; set; }

        [JsonPropertyName("can_mount_drive")]
        public bool CanMountDrive { get; set; }

        [JsonPropertyName("winfsp")]
        public DoctorWinFspInfo WinFsp { get; set; } = new DoctorWinFspInfo();

        [JsonPropertyName("source")]
        public DoctorSourceInfo? Source { get; set; }

        [JsonPropertyName("overlay")]
        public DoctorOverlayInfo? Overlay { get; set; }
    }

    public sealed class DoctorWinFspInfo
    {
        [JsonPropertyName("available")]
        public bool Available { get; set; }

        [JsonPropertyName("dll_path")]
        public string DllPath { get; set; } = string.Empty;

        [JsonPropertyName("version")]
        public string Version { get; set; } = string.Empty;
    }

    public sealed class DoctorSourceInfo
    {
        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("requested_partition")]
        public uint RequestedPartition { get; set; }

        [JsonPropertyName("readable")]
        public bool Readable { get; set; }

        [JsonPropertyName("requires_elevation")]
        public bool RequiresElevation { get; set; }

        [JsonPropertyName("partition_found")]
        public bool PartitionFound { get; set; }

        [JsonPropertyName("error")]
        public string Error { get; set; } = string.Empty;

        [JsonPropertyName("partition")]
        public PartitionInfo? Partition { get; set; }
    }

    public sealed class DoctorOverlayInfo
    {
        [JsonPropertyName("upper_dir")]
        public string UpperDir { get; set; } = string.Empty;

        [JsonPropertyName("case_sensitive")]
        public bool CaseSensitive { get; set; }

        [JsonPropertyName("status")]
        public string Status { get; set; } = string.Empty;

        [JsonPropertyName("can_mount")]
        public bool CanMount { get; set; }

        [JsonPropertyName("marker_version")]
        public int MarkerVersion { get; set; }

        [JsonPropertyName("expected_id")]
        public string ExpectedId { get; set; } = string.Empty;

        [JsonPropertyName("found_id")]
        public string FoundId { get; set; } = string.Empty;

        [JsonPropertyName("reason")]
        public string Reason { get; set; } = string.Empty;
    }

    public sealed class OverlayDiffEntry
    {
        [JsonPropertyName("kind")]
        public string Kind { get; set; } = string.Empty;

        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("is_dir")]
        public bool IsDir { get; set; }

        [JsonPropertyName("size")]
        public ulong Size { get; set; }

        [JsonPropertyName("mode")]
        public uint Mode { get; set; }

        [JsonPropertyName("uid")]
        public uint Uid { get; set; }

        [JsonPropertyName("gid")]
        public uint Gid { get; set; }

        [JsonPropertyName("mtime")]
        public ulong MtimeUnix { get; set; }

        [JsonIgnore]
        public string KindBadge => Kind switch
        {
            "added" => "+",
            "modified" => "M",
            "deleted" => "-",
            "opaque_dir" => "O",
            _ => "U",
        };

        [JsonIgnore]
        public string SizeFormatted => IsDir || Kind == "deleted" || Kind == "opaque_dir"
            ? "-"
            : ByteFormatter.Format(Size);
    }

    public sealed class OverlayDiffSummary
    {
        [JsonPropertyName("added_count")]
        public ulong AddedCount { get; set; }

        [JsonPropertyName("modified_count")]
        public ulong ModifiedCount { get; set; }

        [JsonPropertyName("deleted_count")]
        public ulong DeletedCount { get; set; }

        [JsonPropertyName("opaque_dir_count")]
        public ulong OpaqueDirCount { get; set; }

        [JsonPropertyName("upper_count")]
        public ulong UpperCount { get; set; }

        [JsonPropertyName("file_count")]
        public ulong FileCount { get; set; }

        [JsonPropertyName("dir_count")]
        public ulong DirCount { get; set; }

        [JsonPropertyName("total_upper_bytes")]
        public ulong TotalUpperBytes { get; set; }
    }

    public sealed class OverlayStatusResponse
    {
        [JsonPropertyName("action")]
        public string Action { get; set; } = string.Empty;

        [JsonPropertyName("upper_dir")]
        public string UpperDir { get; set; } = string.Empty;

        [JsonPropertyName("lower_source")]
        public string LowerSource { get; set; } = string.Empty;

        [JsonPropertyName("partition")]
        public uint Partition { get; set; }

        [JsonPropertyName("case_sensitive")]
        public bool CaseSensitive { get; set; }

        [JsonPropertyName("upper_check")]
        public DoctorOverlayInfo UpperCheck { get; set; } = new DoctorOverlayInfo();

        [JsonPropertyName("summary")]
        public OverlayDiffSummary Summary { get; set; } = new OverlayDiffSummary();

        [JsonPropertyName("entries")]
        public List<OverlayDiffEntry> Entries { get; set; } = new List<OverlayDiffEntry>();
    }

    public sealed class OverlayValidateIssue
    {
        [JsonPropertyName("severity")]
        public string Severity { get; set; } = string.Empty;

        [JsonPropertyName("code")]
        public string Code { get; set; } = string.Empty;

        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("message")]
        public string Message { get; set; } = string.Empty;
    }

    public sealed class OverlayValidateResponse
    {
        [JsonPropertyName("action")]
        public string Action { get; set; } = string.Empty;

        [JsonPropertyName("upper_dir")]
        public string UpperDir { get; set; } = string.Empty;

        [JsonPropertyName("valid")]
        public bool Valid { get; set; }

        [JsonPropertyName("error_count")]
        public ulong ErrorCount { get; set; }

        [JsonPropertyName("warning_count")]
        public ulong WarningCount { get; set; }

        [JsonPropertyName("issues")]
        public List<OverlayValidateIssue> Issues { get; set; } = new List<OverlayValidateIssue>();
    }

    public sealed class OverlayExportResponse
    {
        [JsonPropertyName("action")]
        public string Action { get; set; } = string.Empty;

        [JsonPropertyName("upper_dir")]
        public string UpperDir { get; set; } = string.Empty;

        [JsonPropertyName("output_path")]
        public string OutputPath { get; set; } = string.Empty;

        [JsonPropertyName("archive_bytes")]
        public ulong ArchiveBytes { get; set; }

        [JsonPropertyName("exported_entries")]
        public ulong ExportedEntries { get; set; }

        [JsonPropertyName("summary")]
        public OverlayDiffSummary Summary { get; set; } = new OverlayDiffSummary();
    }

    public sealed class OverlayResetResponse
    {
        [JsonPropertyName("action")]
        public string Action { get; set; } = string.Empty;

        [JsonPropertyName("upper_dir")]
        public string UpperDir { get; set; } = string.Empty;

        [JsonPropertyName("kept_marker")]
        public bool KeptMarker { get; set; }

        [JsonPropertyName("removed_entries")]
        public ulong RemovedEntries { get; set; }
    }

    public sealed class FsckLiteFinding
    {
        [JsonPropertyName("severity")]
        public string Severity { get; set; } = string.Empty;

        [JsonPropertyName("category")]
        public string Category { get; set; } = string.Empty;

        [JsonPropertyName("code")]
        public string Code { get; set; } = string.Empty;

        [JsonPropertyName("inode")]
        public ulong Inode { get; set; }

        [JsonPropertyName("block")]
        public ulong Block { get; set; }

        [JsonPropertyName("message")]
        public string Message { get; set; } = string.Empty;
    }

    public sealed class FsckLiteReport
    {
        [JsonPropertyName("source")]
        public string Source { get; set; } = string.Empty;

        [JsonPropertyName("partition")]
        public uint Partition { get; set; }

        [JsonPropertyName("healthy")]
        public bool Healthy { get; set; }

        [JsonPropertyName("fs_kind")]
        public string FsKind { get; set; } = string.Empty;

        [JsonPropertyName("volume_label")]
        public string VolumeLabel { get; set; } = string.Empty;

        [JsonPropertyName("uuid")]
        public string Uuid { get; set; } = string.Empty;

        [JsonPropertyName("block_size")]
        public uint BlockSize { get; set; }

        [JsonPropertyName("inode_size")]
        public uint InodeSize { get; set; }

        [JsonPropertyName("blocks_count")]
        public ulong BlocksCount { get; set; }

        [JsonPropertyName("inodes_count")]
        public ulong InodesCount { get; set; }

        [JsonPropertyName("group_count")]
        public uint GroupCount { get; set; }

        [JsonPropertyName("checked_inodes")]
        public ulong CheckedInodes { get; set; }

        [JsonPropertyName("checked_extents")]
        public ulong CheckedExtents { get; set; }

        [JsonPropertyName("checked_dirs")]
        public ulong CheckedDirs { get; set; }

        [JsonPropertyName("checked_dirents")]
        public ulong CheckedDirents { get; set; }

        [JsonPropertyName("orphan_count")]
        public ulong OrphanCount { get; set; }

        [JsonPropertyName("duplicate_blocks")]
        public ulong DuplicateBlocks { get; set; }

        [JsonPropertyName("journal_state")]
        public string JournalState { get; set; } = string.Empty;

        [JsonPropertyName("error_count")]
        public ulong ErrorCount { get; set; }

        [JsonPropertyName("warning_count")]
        public ulong WarningCount { get; set; }

        [JsonPropertyName("info_count")]
        public ulong InfoCount { get; set; }

        [JsonPropertyName("findings")]
        public List<FsckLiteFinding> Findings { get; set; } = new List<FsckLiteFinding>();
    }

    public static class ByteFormatter
    {
        public static string Format(ulong bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB", "PB" };
            double val = bytes;
            int idx = 0;
            while (val >= 1024.0 && idx < units.Length - 1)
            {
                val /= 1024.0;
                idx++;
            }
            if (idx == 0)
            {
                return $"{bytes} B";
            }
            return val >= 100.0
                ? string.Format(CultureInfo.InvariantCulture, "{0:0.0} {1}", val, units[idx])
                : string.Format(CultureInfo.InvariantCulture, "{0:0.00} {1}", val, units[idx]);
        }

        public static string FormatRate(double bytesPerSec)
        {
            if (bytesPerSec <= 0.5)
            {
                return "0 B/s";
            }
            string[] units = { "B/s", "KB/s", "MB/s", "GB/s" };
            double val = bytesPerSec;
            int idx = 0;
            while (val >= 1024.0 && idx < units.Length - 1)
            {
                val /= 1024.0;
                idx++;
            }
            return string.Format(CultureInfo.InvariantCulture, "{0:0.0} {1}", val, units[idx]);
        }
    }
}
