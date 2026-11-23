using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.Models
{
    public enum MountMode
    {
        ReadOnly = 0,
        Overlay = 1,
        ReadWrite = 2,
    }

    public enum UpperDirCheckStatus
    {
        None = 0,
        WillCreate = 1,
        EmptyReady = 2,
        MatchingExisting = 3,
        NonEmptyUnmarked = 4,
        MismatchedLower = 5,
        InvalidPath = 6,
    }

    public sealed class UpperDirCheckResult
    {
        public UpperDirCheckStatus Status { get; init; }
        public string Message { get; init; } = string.Empty;
        public bool CanMount { get; init; }
    }

    public sealed class MountRequest
    {
        public string SourcePath { get; set; } = string.Empty;
        public uint PartitionNumber { get; set; }
        public string MountPoint { get; set; } = string.Empty;
        public MountMode Mode { get; set; } = MountMode.ReadOnly;
        public string OverlayUpperDir { get; set; } = string.Empty;
        public bool EnableDirectIo { get; set; } = true;
        public bool CaseSensitive { get; set; }
        public uint CacheBlocks { get; set; } = 2048;
        public bool RoFallbackOnDirty { get; set; } = true;
        public bool ForceDirty { get; set; }
        public bool EnableDebugLog { get; set; }
        public string VolumeLabel { get; set; } = string.Empty;
        public bool AutoOpenExplorer { get; set; } = true;
        public string TraceOutputPath { get; set; } = string.Empty;
    }

    public sealed class ActiveMountInfo : INotifyPropertyChanged
    {
        private ulong _readBytes;
        private ulong _writeBytes;
        private long _readOps;
        private long _writeOps;
        private double _readBytesPerSec;
        private double _writeBytesPerSec;
        private bool _isUnmounting;
        private string _statusText = LocalizationService.Get("Str_MountStatusActive");

        public string Id { get; init; } = string.Empty;
        public string SourcePath { get; init; } = string.Empty;
        public uint PartitionNumber { get; init; }
        public string MountPoint { get; init; } = string.Empty;
        public string VolumeLabel { get; init; } = string.Empty;
        public string ModeDisplay { get; init; } = string.Empty;
        public bool IsReadOnly { get; init; }
        public bool IsOverlay { get; init; }
        public string OverlayUpperDir { get; init; } = string.Empty;
        public bool DirectIoEnabled { get; init; }
        public bool FellBackToRo { get; init; }
        public int ProcessId { get; init; }
        public DateTime StartedAt { get; init; } = DateTime.Now;
        public string TraceOutputPath { get; init; } = string.Empty;
        public bool IsTracing => !string.IsNullOrWhiteSpace(TraceOutputPath);

        public ulong ReadBytes
        {
            get => _readBytes;
            set
            {
                if (_readBytes != value)
                {
                    _readBytes = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(ReadBytesFormatted));
                }
            }
        }

        public ulong WriteBytes
        {
            get => _writeBytes;
            set
            {
                if (_writeBytes != value)
                {
                    _writeBytes = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(WriteBytesFormatted));
                }
            }
        }

        public long ReadOps
        {
            get => _readOps;
            set
            {
                if (_readOps != value)
                {
                    _readOps = value;
                    OnPropertyChanged();
                }
            }
        }

        public long WriteOps
        {
            get => _writeOps;
            set
            {
                if (_writeOps != value)
                {
                    _writeOps = value;
                    OnPropertyChanged();
                }
            }
        }

        public double ReadBytesPerSec
        {
            get => _readBytesPerSec;
            set
            {
                if (Math.Abs(_readBytesPerSec - value) > 0.01)
                {
                    _readBytesPerSec = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(ReadRateFormatted));
                }
            }
        }

        public double WriteBytesPerSec
        {
            get => _writeBytesPerSec;
            set
            {
                if (Math.Abs(_writeBytesPerSec - value) > 0.01)
                {
                    _writeBytesPerSec = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(WriteRateFormatted));
                }
            }
        }

        public bool IsUnmounting
        {
            get => _isUnmounting;
            set
            {
                if (_isUnmounting != value)
                {
                    _isUnmounting = value;
                    OnPropertyChanged();
                }
            }
        }

        public string StatusText
        {
            get => _statusText;
            set
            {
                if (_statusText != value)
                {
                    _statusText = value;
                    OnPropertyChanged();
                }
            }
        }

        public string DisplayTitle =>
            string.IsNullOrWhiteSpace(VolumeLabel)
                ? $"{MountPoint}  (Part {PartitionNumber})"
                : $"{MountPoint}  ({VolumeLabel})";

        public string SourceSummary =>
            PartitionNumber == 0 ? SourcePath : $"{SourcePath} [Partition {PartitionNumber}]";

        public string ReadBytesFormatted => ByteFormatter.Format(ReadBytes);
        public string WriteBytesFormatted => ByteFormatter.Format(WriteBytes);
        public string ReadRateFormatted => ByteFormatter.FormatRate(ReadBytesPerSec);
        public string WriteRateFormatted => ByteFormatter.FormatRate(WriteBytesPerSec);
        public string DirectIoBadge => DirectIoEnabled ? "Direct I/O: ON" : "Direct I/O: OFF";

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
