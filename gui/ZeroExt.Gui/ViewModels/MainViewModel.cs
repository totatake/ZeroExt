using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using ZeroExt.Gui.Models;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.ViewModels
{
    public sealed class PartitionItem : ViewModelBase
    {
        private ActiveMountInfo? _activeMount;
        private bool _isSelected;

        public DiskInfo ParentDisk { get; }
        public PartitionInfo Info { get; }

        public PartitionItem(DiskInfo parentDisk, PartitionInfo info)
        {
            ParentDisk = parentDisk;
            Info = info;
        }

        public bool IsSelected
        {
            get => _isSelected;
            set => SetProperty(ref _isSelected, value);
        }

        public string UniqueKey => $"{ParentDisk.DevicePath}#{Info.PartitionNumber}";

        public ActiveMountInfo? ActiveMount
        {
            get => _activeMount;
            set
            {
                if (SetProperty(ref _activeMount, value))
                {
                    OnPropertyChanged(nameof(IsMounted));
                    OnPropertyChanged(nameof(MountedBadgeText));
                }
            }
        }

        public bool IsMounted => ActiveMount != null;

        public string MountedBadgeText => ActiveMount != null ? $"{ActiveMount.MountPoint} マウント中" : string.Empty;

        public string DisplayTitle => Info.DisplayName;

        public string PartitionSubtitle =>
            Info.PartitionNumber == 0
                ? $"Raw  |  {ByteFormatter.Format(Info.EffectiveTotalBytes)}"
                : $"Part {Info.PartitionNumber}  |  {ByteFormatter.Format(Info.EffectiveTotalBytes)}";

        public string FsBadgeText =>
            string.IsNullOrWhiteSpace(Info.FsKind) || Info.FsKind == "unknown"
                ? (string.IsNullOrWhiteSpace(Info.TypeDescription) ? "unknown" : Info.TypeDescription)
                : Info.FsKind;

        public string JournalBadgeText => Info.JournalDisplayText;

        public double UsedPercent => Info.UsedPercent;

        public string CapacitySummary =>
            Info.IsExtFs && Info.EffectiveTotalBytes > 0
                ? $"使用中 {ByteFormatter.Format(Info.UsedBytes)} / {ByteFormatter.Format(Info.EffectiveTotalBytes)} (空き {ByteFormatter.Format(Info.FreeBytes)})"
                : ByteFormatter.Format(Info.SizeBytes);
    }

    public sealed class DiskGroupItem : ViewModelBase
    {
        private bool _isExpanded = true;

        public DiskInfo Disk { get; }
        public ObservableCollection<PartitionItem> AllPartitions { get; } = new ObservableCollection<PartitionItem>();
        public ObservableCollection<PartitionItem> VisiblePartitions { get; } = new ObservableCollection<PartitionItem>();

        public DiskGroupItem(DiskInfo disk)
        {
            Disk = disk;
            foreach (PartitionInfo part in disk.Partitions)
            {
                AllPartitions.Add(new PartitionItem(disk, part));
            }
        }

        public bool IsImageFile => Disk.IsImageFile;
        public bool Accessible => Disk.Accessible;
        public string DisplayTitle => Disk.DisplayTitle;
        public string SubtitleText => Disk.SubtitleText;

        public bool IsExpanded
        {
            get => _isExpanded;
            set => SetProperty(ref _isExpanded, value);
        }

        public int HiddenNonExtCount => AllPartitions.Count - VisiblePartitions.Count;
        public bool HasHiddenNonExt => HiddenNonExtCount > 0;
        public string HiddenSummary => $"非 ext パーティション {HiddenNonExtCount} 件を非表示中";
        public bool HasNoVisibleExtPartitions => Accessible && VisiblePartitions.Count == 0;

        public void ApplyFilter(bool showNonExt)
        {
            VisiblePartitions.Clear();
            foreach (PartitionItem p in AllPartitions)
            {
                if (showNonExt || p.Info.IsExtFs)
                {
                    VisiblePartitions.Add(p);
                }
            }
            OnPropertyChanged(nameof(HiddenNonExtCount));
            OnPropertyChanged(nameof(HasHiddenNonExt));
            OnPropertyChanged(nameof(HiddenSummary));
            OnPropertyChanged(nameof(HasNoVisibleExtPartitions));
        }
    }

    public sealed class MainViewModel : ViewModelBase, IDisposable
    {
        private readonly SettingsService _settingsService;
        private readonly GuiSettings _settings;
        private readonly ZeroExtCoreClient _coreClient;
        private readonly MountSessionManager _mountManager;
        private readonly Action<Action> _marshalToUi;
        private readonly Action<bool> _applyThemeCallback;
        private string? _coreExeOverride;
        private string? _coreExePath;

        private bool _isElevated;
        private bool _isWinFspAvailable = true;
        private bool _isScanning;
        private bool _isMounting;
        private bool _showNonExtPartitions;
        private bool _isDarkTheme;
        private int _selectedTabIndex;
        private string _statusMessage = "準備完了";

        private PartitionItem? _selectedPartition;
        private MountMode _selectedMode = MountMode.ReadOnly;
        private bool _useFolderMountPoint;
        private string _selectedDriveLetter = "M:";
        private string _mountFolderPath = string.Empty;
        private string _overlayUpperDir = string.Empty;
        private UpperDirCheckResult _upperDirCheck = new UpperDirCheckResult
        {
            Status = UpperDirCheckStatus.None,
            Message = "Overlay の書き込み先フォルダを指定してください。",
            CanMount = false,
        };
        private bool _enableDirectIo = true;
        private string _cacheBlocksText = "2048";
        private bool _roFallbackOnDirty = true;
        private bool _forceDirty;
        private bool _enableDebugLog;
        private bool _autoOpenExplorer = true;
        private bool _minimizeToTrayWhenMounted = true;

        private string _currentBrowsePath = "/";
        private bool _isBrowseLoading;
        private string? _browseError;
        private LsEntry? _selectedBrowseEntry;

        public ObservableCollection<DiskGroupItem> DiskGroups { get; } = new ObservableCollection<DiskGroupItem>();
        public ObservableCollection<string> AvailableDriveLetters { get; } = new ObservableCollection<string>();
        public ObservableCollection<LsEntry> BrowseEntries { get; } = new ObservableCollection<LsEntry>();
        public ObservableCollection<string> LogEntries { get; } = new ObservableCollection<string>();
        public ObservableCollection<ActiveMountInfo> ActiveMounts => _mountManager.ActiveMounts;

        public MountSessionManager MountManager => _mountManager;
        public ZeroExtCoreClient CoreClient => _coreClient;

        public AsyncRelayCommand RefreshDisksCommand { get; }
        public AsyncRelayCommand MountCommand { get; }
        public AsyncRelayCommand UnmountSelectedCommand { get; }
        public AsyncRelayCommand UnmountItemCommand { get; }
        public AsyncRelayCommand UnmountAllCommand { get; }
        public AsyncRelayCommand RefreshBrowseCommand { get; }
        public AsyncRelayCommand NavigateUpBrowseCommand { get; }
        public RelayCommand ToggleThemeCommand { get; }
        public RelayCommand SetAutoOverlayPathCommand { get; }
        public RelayCommand ClearLogsCommand { get; }
        public RelayCommand OpenMountedExplorerCommand { get; }
        public RelayCommand OpenUpperDirCommand { get; }
        public RelayCommand RemoveImageCommand { get; }

        public Action<string, string, string?>? ShowErrorNotification { get; set; }
        public Func<ActiveMountInfo, bool>? ConfirmForceKillCallback { get; set; }

        public MainViewModel(
            SettingsService settingsService,
            Action<bool>? applyThemeCallback = null,
            string? coreExeOverride = null,
            Action<Action>? marshalToUi = null)
        {
            _settingsService = settingsService ?? throw new ArgumentNullException(nameof(settingsService));
            _applyThemeCallback = applyThemeCallback ?? (_ => { });
            _coreExeOverride = coreExeOverride;

            if (marshalToUi != null)
            {
                _marshalToUi = marshalToUi;
            }
            else
            {
                System.Threading.SynchronizationContext? sync = System.Threading.SynchronizationContext.Current;
                _marshalToUi = sync != null
                    ? (act => sync.Post(_ => act(), null))
                    : (act => act());
            }

            _settings = _settingsService.Load();
            _isDarkTheme = string.Equals(_settings.Theme, "Dark", StringComparison.OrdinalIgnoreCase);
            _showNonExtPartitions = _settings.ShowNonExtPartitions;
            _autoOpenExplorer = _settings.AutoOpenExplorerOnMount;
            _minimizeToTrayWhenMounted = _settings.MinimizeToTrayWhenMounted;
            _isElevated = ElevationHelper.IsCurrentProcessElevated();
            _coreExePath = CoreExecutableLocator.Locate(_coreExeOverride, _settings.CoreExePath);

            _coreClient = new ZeroExtCoreClient(() => _coreExePath, AppendLog);
            _mountManager = new MountSessionManager(() => _coreExePath, AppendLog, _marshalToUi);
            _mountManager.ActiveMounts.CollectionChanged += OnActiveMountsChanged;

            RefreshDisksCommand = new AsyncRelayCommand(RefreshAllAsync, () => !IsScanning && HasCoreExe);
            MountCommand = new AsyncRelayCommand(ExecuteMountAsync, () => CanMount && !IsMounting);
            UnmountSelectedCommand = new AsyncRelayCommand(
                () => SelectedPartition?.ActiveMount != null
                    ? UnmountItemAsync(SelectedPartition.ActiveMount)
                    : Task.CompletedTask,
                () => SelectedPartition?.ActiveMount != null);
            UnmountItemCommand = new AsyncRelayCommand(
                p => p is ActiveMountInfo m ? UnmountItemAsync(m) : Task.CompletedTask);
            UnmountAllCommand = new AsyncRelayCommand(
                () => _mountManager.UnmountAllAsync(),
                () => ActiveMounts.Count > 0);
            RefreshBrowseCommand = new AsyncRelayCommand(
                () => LoadBrowseDirectoryAsync(CurrentBrowsePath),
                () => SelectedPartition != null && SelectedPartition.Info.IsExtFs && !IsBrowseLoading && HasCoreExe);
            NavigateUpBrowseCommand = new AsyncRelayCommand(
                () => LoadBrowseDirectoryAsync(GetParentUnixPath(CurrentBrowsePath)),
                () => CanNavigateUpBrowse && !IsBrowseLoading && HasCoreExe);

            ToggleThemeCommand = new RelayCommand(() => IsDarkTheme = !IsDarkTheme);
            SetAutoOverlayPathCommand = new RelayCommand(ApplySuggestedOverlayPath, () => SelectedPartition != null);
            ClearLogsCommand = new RelayCommand(() => LogEntries.Clear());
            OpenMountedExplorerCommand = new RelayCommand(p =>
            {
                if (p is ActiveMountInfo m)
                {
                    ElevationHelper.OpenInExplorer(m.MountPoint);
                }
                else if (SelectedPartition?.ActiveMount != null)
                {
                    ElevationHelper.OpenInExplorer(SelectedPartition.ActiveMount.MountPoint);
                }
            });
            OpenUpperDirCommand = new RelayCommand(p =>
            {
                string? dir = p as string ?? OverlayUpperDir;
                if (!string.IsNullOrWhiteSpace(dir) && Directory.Exists(dir))
                {
                    ElevationHelper.OpenInExplorer(dir);
                }
            });
            RemoveImageCommand = new RelayCommand(p =>
            {
                if (p is DiskGroupItem group && group.IsImageFile)
                {
                    RemoveRegisteredImage(group.Disk.DevicePath);
                }
            });

            RefreshAvailableDriveLetters();
        }

        public bool HasCoreExe => !string.IsNullOrWhiteSpace(_coreExePath) && File.Exists(_coreExePath);
        public string CoreExeDisplay => _coreExePath ?? "見つかりません (build\\zeroext.exe をビルドしてください)";

        public bool IsElevated
        {
            get => _isElevated;
            private set => SetProperty(ref _isElevated, value);
        }

        public bool IsWinFspAvailable
        {
            get => _isWinFspAvailable;
            private set
            {
                if (SetProperty(ref _isWinFspAvailable, value))
                {
                    OnPropertyChanged(nameof(WinFspBannerVisible));
                    OnPropertyChanged(nameof(CanMount));
                    OnPropertyChanged(nameof(MountValidationMessage));
                    MountCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public bool WinFspBannerVisible => HasCoreExe && !IsWinFspAvailable;

        public bool NonElevatedNoticeVisible => !IsElevated;

        public bool IsScanning
        {
            get => _isScanning;
            private set
            {
                if (SetProperty(ref _isScanning, value))
                {
                    RefreshDisksCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public bool IsMounting
        {
            get => _isMounting;
            private set
            {
                if (SetProperty(ref _isMounting, value))
                {
                    OnPropertyChanged(nameof(MountButtonText));
                    MountCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public bool ShowNonExtPartitions
        {
            get => _showNonExtPartitions;
            set
            {
                if (SetProperty(ref _showNonExtPartitions, value))
                {
                    _settings.ShowNonExtPartitions = value;
                    SaveSettingsSafe();
                    foreach (DiskGroupItem g in DiskGroups)
                    {
                        g.ApplyFilter(value);
                    }
                    EnsureValidSelection();
                }
            }
        }

        public bool IsDarkTheme
        {
            get => _isDarkTheme;
            set
            {
                if (SetProperty(ref _isDarkTheme, value))
                {
                    _settings.Theme = value ? "Dark" : "Light";
                    SaveSettingsSafe();
                    _applyThemeCallback(value);
                    OnPropertyChanged(nameof(ThemeToggleLabel));
                }
            }
        }

        public string ThemeToggleLabel => IsDarkTheme ? "☀ ライト" : "🌙 ダーク";

        public int SelectedTabIndex
        {
            get => _selectedTabIndex;
            set
            {
                if (SetProperty(ref _selectedTabIndex, value))
                {
                    if (value == 2 && SelectedPartition != null && SelectedPartition.Info.IsExtFs &&
                        BrowseEntries.Count == 0 && string.IsNullOrEmpty(BrowseError))
                    {
                        _ = LoadBrowseDirectoryAsync(CurrentBrowsePath);
                    }
                }
            }
        }

        public string StatusMessage
        {
            get => _statusMessage;
            set => SetProperty(ref _statusMessage, value);
        }

        public PartitionItem? SelectedPartition
        {
            get => _selectedPartition;
            set
            {
                PartitionItem? prev = _selectedPartition;
                if (SetProperty(ref _selectedPartition, value))
                {
                    if (prev != null) prev.IsSelected = false;
                    if (value != null) value.IsSelected = true;
                    OnPropertyChanged(nameof(HasSelectedPartition));
                    OnPropertyChanged(nameof(HasNoSelectedPartition));
                    OnPropertyChanged(nameof(SelectedIsExtFs));
                    OnPropertyChanged(nameof(SelectedIsDirty));
                    OnPropertyChanged(nameof(ShowDirtyRwWarning));
                    OnPropertyChanged(nameof(SelectedVolumeTitle));
                    OnPropertyChanged(nameof(SelectedVolumeSubtitle));
                    OnPropertyChanged(nameof(ExpectedLowerIdDisplay));
                    LoadPresetForSelectedPartition();
                    RefreshUpperDirValidation();
                    NotifyMountStateChanged();
                    SetAutoOverlayPathCommand.RaiseCanExecuteChanged();
                    RefreshBrowseCommand.RaiseCanExecuteChanged();
                    UnmountSelectedCommand.RaiseCanExecuteChanged();

                    BrowseEntries.Clear();
                    BrowseError = null;
                    CurrentBrowsePath = "/";
                    if (SelectedTabIndex == 2 && value != null && value.Info.IsExtFs)
                    {
                        _ = LoadBrowseDirectoryAsync("/");
                    }
                }
            }
        }

        public bool HasSelectedPartition => SelectedPartition != null;
        public bool HasNoSelectedPartition => SelectedPartition == null;
        public bool SelectedIsExtFs => SelectedPartition?.Info.IsExtFs ?? false;
        public bool SelectedIsDirty => SelectedPartition?.Info.IsDirty ?? false;

        public string SelectedVolumeTitle =>
            SelectedPartition == null
                ? "パーティションまたはイメージを選択してください"
                : $"{SelectedPartition.DisplayTitle}  ({SelectedPartition.FsBadgeText})";

        public string SelectedVolumeSubtitle =>
            SelectedPartition == null
                ? "左のリストから ext2/ext3/ext4 ボリュームを選ぶか、.img ファイルをドラッグ＆ドロップします。"
                : $"{SelectedPartition.ParentDisk.DevicePath}  |  {SelectedPartition.PartitionSubtitle}  |  UUID: {(string.IsNullOrWhiteSpace(SelectedPartition.Info.Uuid) ? "-" : SelectedPartition.Info.Uuid)}";

        public string ExpectedLowerIdDisplay =>
            SelectedPartition?.Info.ExpectedOverlayLowerId ?? "-";

        public MountMode SelectedMode
        {
            get => _selectedMode;
            set
            {
                if (SetProperty(ref _selectedMode, value))
                {
                    OnPropertyChanged(nameof(IsReadOnlyMode));
                    OnPropertyChanged(nameof(IsOverlayMode));
                    OnPropertyChanged(nameof(IsReadWriteMode));
                    OnPropertyChanged(nameof(DirectIoEditable));
                    OnPropertyChanged(nameof(EffectiveDirectIo));
                    OnPropertyChanged(nameof(ShowDirtyRwWarning));
                    RefreshUpperDirValidation();
                    NotifyMountStateChanged();
                }
            }
        }

        public bool IsReadOnlyMode => SelectedMode == MountMode.ReadOnly;
        public bool IsOverlayMode => SelectedMode == MountMode.Overlay;
        public bool IsReadWriteMode => SelectedMode == MountMode.ReadWrite;

        public bool DirectIoEditable => SelectedMode != MountMode.ReadWrite;
        public bool EffectiveDirectIo => DirectIoEditable && EnableDirectIo;

        public bool ShowDirtyRwWarning => SelectedIsDirty && SelectedMode == MountMode.ReadWrite;

        public bool UseFolderMountPoint
        {
            get => _useFolderMountPoint;
            set
            {
                if (SetProperty(ref _useFolderMountPoint, value))
                {
                    OnPropertyChanged(nameof(UseDriveLetterMountPoint));
                    NotifyMountStateChanged();
                }
            }
        }

        public bool UseDriveLetterMountPoint => !UseFolderMountPoint;

        public string SelectedDriveLetter
        {
            get => _selectedDriveLetter;
            set
            {
                if (SetProperty(ref _selectedDriveLetter, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public string MountFolderPath
        {
            get => _mountFolderPath;
            set
            {
                if (SetProperty(ref _mountFolderPath, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public string OverlayUpperDir
        {
            get => _overlayUpperDir;
            set
            {
                if (SetProperty(ref _overlayUpperDir, value))
                {
                    RefreshUpperDirValidation();
                    NotifyMountStateChanged();
                }
            }
        }

        public UpperDirCheckResult UpperDirCheck
        {
            get => _upperDirCheck;
            private set => SetProperty(ref _upperDirCheck, value);
        }

        public bool EnableDirectIo
        {
            get => _enableDirectIo;
            set
            {
                if (SetProperty(ref _enableDirectIo, value))
                {
                    OnPropertyChanged(nameof(EffectiveDirectIo));
                    NotifyMountStateChanged();
                }
            }
        }

        public string CacheBlocksText
        {
            get => _cacheBlocksText;
            set
            {
                if (SetProperty(ref _cacheBlocksText, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool RoFallbackOnDirty
        {
            get => _roFallbackOnDirty;
            set
            {
                if (SetProperty(ref _roFallbackOnDirty, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool ForceDirty
        {
            get => _forceDirty;
            set
            {
                if (SetProperty(ref _forceDirty, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool EnableDebugLog
        {
            get => _enableDebugLog;
            set
            {
                if (SetProperty(ref _enableDebugLog, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool AutoOpenExplorer
        {
            get => _autoOpenExplorer;
            set
            {
                if (SetProperty(ref _autoOpenExplorer, value))
                {
                    _settings.AutoOpenExplorerOnMount = value;
                    SaveSettingsSafe();
                }
            }
        }

        public bool MinimizeToTrayWhenMounted
        {
            get => _minimizeToTrayWhenMounted;
            set
            {
                if (SetProperty(ref _minimizeToTrayWhenMounted, value))
                {
                    _settings.MinimizeToTrayWhenMounted = value;
                    SaveSettingsSafe();
                }
            }
        }

        public string ActiveMountPointTarget =>
            UseFolderMountPoint ? MountFolderPath.Trim() : SelectedDriveLetter;

        public string CliPreviewCommand
        {
            get
            {
                if (SelectedPartition == null)
                {
                    return "zeroext.exe mount --source <source> --mountpoint M:";
                }
                return ZeroExtArgBuilder.BuildMountPreviewCommand(BuildCurrentMountRequest());
            }
        }

        public string MountButtonText =>
            IsMounting
                ? "マウント中..."
                : SelectedMode switch
                {
                    MountMode.Overlay => "Overlay でマウントする",
                    MountMode.ReadWrite => "Read-Write でマウントする",
                    _ => "Read-Only でマウントする",
                };

        public string MountValidationMessage
        {
            get
            {
                if (!HasCoreExe) return "zeroext.exe が見つかりません。";
                if (!IsWinFspAvailable) return "WinFsp ランタイムが未インストールのためマウントできません（右上の WinFsp ガイドを参照）。";
                if (SelectedPartition == null) return "マウント対象のパーティションまたはイメージを選択してください。";
                if (!SelectedPartition.Info.IsExtFs) return "選択中のパーティションは ext2 / ext3 / ext4 ではありません。";
                if (SelectedPartition.IsMounted) return $"既に {SelectedPartition.ActiveMount?.MountPoint} にマウント済みです。";

                if (UseFolderMountPoint)
                {
                    var (valid, msg) = DriveLetterService.ValidateMountFolder(MountFolderPath);
                    if (!valid) return msg;
                }
                else
                {
                    if (string.IsNullOrWhiteSpace(SelectedDriveLetter))
                    {
                        return "利用可能なドライブレターがありません。";
                    }
                    if (ActiveMounts.Any(m => string.Equals(
                        ZeroExtArgBuilder.NormalizeMountPointArg(m.MountPoint),
                        ZeroExtArgBuilder.NormalizeMountPointArg(SelectedDriveLetter),
                        StringComparison.OrdinalIgnoreCase)))
                    {
                        return $"ドライブレター {SelectedDriveLetter} は既にマウントに使用中です。";
                    }
                }

                if (SelectedMode == MountMode.Overlay && !UpperDirCheck.CanMount)
                {
                    return UpperDirCheck.Message;
                }

                if (!uint.TryParse(CacheBlocksText, out uint cb) || cb == 0)
                {
                    return "キャッシュブロック数は 1 以上の整数を指定してください。";
                }

                return string.Empty;
            }
        }

        public bool CanMount => string.IsNullOrEmpty(MountValidationMessage);

        public int ActiveMountCount => ActiveMounts.Count;
        public string ActiveMountsTabHeader => ActiveMounts.Count > 0
            ? $"アクティブマウント ({ActiveMounts.Count})"
            : "アクティブマウント";

        public string CurrentBrowsePath
        {
            get => _currentBrowsePath;
            set
            {
                if (SetProperty(ref _currentBrowsePath, value))
                {
                    OnPropertyChanged(nameof(CanNavigateUpBrowse));
                    NavigateUpBrowseCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public bool CanNavigateUpBrowse => !string.Equals(CurrentBrowsePath, "/", StringComparison.Ordinal);

        public bool IsBrowseLoading
        {
            get => _isBrowseLoading;
            private set
            {
                if (SetProperty(ref _isBrowseLoading, value))
                {
                    RefreshBrowseCommand.RaiseCanExecuteChanged();
                    NavigateUpBrowseCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public string? BrowseError
        {
            get => _browseError;
            private set => SetProperty(ref _browseError, value);
        }

        public LsEntry? SelectedBrowseEntry
        {
            get => _selectedBrowseEntry;
            set => SetProperty(ref _selectedBrowseEntry, value);
        }

        public void SetCoreExecutablePath(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return;
            _coreExeOverride = path;
            _coreExePath = Path.GetFullPath(path);
            _settings.CoreExePath = _coreExePath;
            SaveSettingsSafe();
            OnPropertyChanged(nameof(HasCoreExe));
            OnPropertyChanged(nameof(CoreExeDisplay));
            OnPropertyChanged(nameof(WinFspBannerVisible));
            NotifyMountStateChanged();
            RefreshDisksCommand.RaiseCanExecuteChanged();
        }

        public async Task RefreshAllAsync()
        {
            if (!HasCoreExe)
            {
                StatusMessage = "zeroext.exe が見つかりません。";
                return;
            }

            IsScanning = true;
            StatusMessage = "ディスクおよび登録済みイメージをスキャンしています...";
            string? previousSelectedKey = SelectedPartition?.UniqueKey;

            try
            {
                ScanResponse physical = await _coreClient.ScanPhysicalDisksAsync();
                IsElevated = physical.IsElevated;
                IsWinFspAvailable = physical.WinFspAvailable;
                OnPropertyChanged(nameof(NonElevatedNoticeVisible));

                var newGroups = new List<DiskGroupItem>();
                foreach (DiskInfo d in physical.Disks)
                {
                    var group = new DiskGroupItem(d);
                    group.ApplyFilter(ShowNonExtPartitions);
                    newGroups.Add(group);
                }

                // Re-inspect registered image files
                var validImages = new List<string>();
                foreach (string imgPath in _settings.RegisteredImages.Distinct(StringComparer.OrdinalIgnoreCase))
                {
                    if (!File.Exists(imgPath))
                    {
                        AppendLog($"[scan] 登録イメージが見つからないため除外: {imgPath}");
                        continue;
                    }
                    try
                    {
                        ScanResponse imgScan = await _coreClient.InspectSourceAsync(imgPath);
                        foreach (DiskInfo d in imgScan.Disks)
                        {
                            var group = new DiskGroupItem(d);
                            group.ApplyFilter(ShowNonExtPartitions);
                            newGroups.Add(group);
                        }
                        validImages.Add(imgPath);
                    }
                    catch (Exception ex)
                    {
                        AppendLog($"[scan] イメージ検査失敗 '{imgPath}': {ex.Message}");
                        validImages.Add(imgPath);
                    }
                }

                if (validImages.Count != _settings.RegisteredImages.Count)
                {
                    _settings.RegisteredImages = validImages;
                    SaveSettingsSafe();
                }

                DiskGroups.Clear();
                foreach (DiskGroupItem g in newGroups)
                {
                    DiskGroups.Add(g);
                }

                SyncActiveMountsToPartitions();
                RefreshAvailableDriveLetters();

                if (!string.IsNullOrEmpty(previousSelectedKey))
                {
                    PartitionItem? match = DiskGroups
                        .SelectMany(g => g.VisiblePartitions)
                        .FirstOrDefault(p => string.Equals(p.UniqueKey, previousSelectedKey, StringComparison.OrdinalIgnoreCase));
                    if (match != null)
                    {
                        SelectedPartition = match;
                    }
                    else
                    {
                        EnsureValidSelection();
                    }
                }
                else
                {
                    EnsureValidSelection();
                }

                int extCount = DiskGroups.SelectMany(g => g.AllPartitions).Count(p => p.Info.IsExtFs);
                StatusMessage = $"スキャン完了: {DiskGroups.Count} ソース / ext2-4 ボリューム {extCount} 件検出";
            }
            catch (Exception ex)
            {
                AppendLog($"[scan] エラー: {ex.Message}");
                StatusMessage = $"スキャンエラー: {ex.Message}";
            }
            finally
            {
                IsScanning = false;
            }
        }

        public async Task AddImageFileAsync(string imagePath, bool autoSelect = true)
        {
            if (string.IsNullOrWhiteSpace(imagePath) || !File.Exists(imagePath))
            {
                return;
            }

            string fullPath = Path.GetFullPath(imagePath);
            if (!_settings.RegisteredImages.Contains(fullPath, StringComparer.OrdinalIgnoreCase))
            {
                _settings.RegisteredImages.Add(fullPath);
                SaveSettingsSafe();
            }

            if (!HasCoreExe) return;

            try
            {
                StatusMessage = $"イメージを読み込んでいます: {Path.GetFileName(fullPath)}";
                ScanResponse imgScan = await _coreClient.InspectSourceAsync(fullPath);
                IsElevated = imgScan.IsElevated;
                IsWinFspAvailable = imgScan.WinFspAvailable;

                DiskGroupItem? existing = DiskGroups.FirstOrDefault(
                    g => g.IsImageFile && string.Equals(g.Disk.DevicePath, fullPath, StringComparison.OrdinalIgnoreCase));
                if (existing != null)
                {
                    DiskGroups.Remove(existing);
                }

                DiskGroupItem? addedGroup = null;
                foreach (DiskInfo d in imgScan.Disks)
                {
                    var group = new DiskGroupItem(d);
                    group.ApplyFilter(ShowNonExtPartitions);
                    DiskGroups.Add(group);
                    addedGroup ??= group;
                }

                SyncActiveMountsToPartitions();
                if (autoSelect && addedGroup != null)
                {
                    PartitionItem? firstExt = addedGroup.VisiblePartitions.FirstOrDefault(p => p.Info.IsExtFs)
                                              ?? addedGroup.VisiblePartitions.FirstOrDefault();
                    if (firstExt != null)
                    {
                        SelectedPartition = firstExt;
                    }
                }

                StatusMessage = $"イメージを読み込みました: {Path.GetFileName(fullPath)}";
            }
            catch (Exception ex)
            {
                AppendLog($"[image] 検査エラー '{fullPath}': {ex.Message}");
                ShowErrorNotification?.Invoke(
                    "イメージファイルを検査できませんでした",
                    ex.Message,
                    "有効な ext2/ext3/ext4 または GPT/MBR ディスクイメージ (.img) であるか確認してください。");
                StatusMessage = "イメージ読み込みに失敗しました";
            }
        }

        public void RemoveRegisteredImage(string imagePath)
        {
            _settings.RegisteredImages.RemoveAll(
                p => string.Equals(p, imagePath, StringComparison.OrdinalIgnoreCase));
            SaveSettingsSafe();

            DiskGroupItem? group = DiskGroups.FirstOrDefault(
                g => g.IsImageFile && string.Equals(g.Disk.DevicePath, imagePath, StringComparison.OrdinalIgnoreCase));
            if (group != null)
            {
                bool wasSelected = SelectedPartition != null && group.AllPartitions.Contains(SelectedPartition);
                DiskGroups.Remove(group);
                if (wasSelected)
                {
                    SelectedPartition = null;
                    EnsureValidSelection();
                }
            }
        }

        public async Task ExecuteMountAsync()
        {
            if (SelectedPartition == null || !CanMount) return;

            MountRequest req = BuildCurrentMountRequest();
            SavePresetForSelectedPartition();

            IsMounting = true;
            StatusMessage = $"{req.MountPoint} へマウントしています...";
            try
            {
                ActiveMountInfo info = await _mountManager.StartMountAsync(req);
                RefreshAvailableDriveLetters();
                StatusMessage = $"{info.MountPoint} にマウントしました ({info.ModeDisplay})";

                if (AutoOpenExplorer)
                {
                    ElevationHelper.OpenInExplorer(info.MountPoint);
                }

                SelectedTabIndex = 1; // Switch to Active Mounts tab for immediate status & controls
            }
            catch (MountFailedException mf)
            {
                StatusMessage = mf.Translated.Title;
                ShowErrorNotification?.Invoke(
                    mf.Translated.Title,
                    mf.Translated.Message + "\n\n対処方法: " + mf.Translated.Suggestion,
                    mf.Translated.RawDetail);
            }
            catch (Exception ex)
            {
                StatusMessage = "マウントに失敗しました";
                ShowErrorNotification?.Invoke("マウントエラー", ex.Message, null);
            }
            finally
            {
                IsMounting = false;
                NotifyMountStateChanged();
            }
        }

        public async Task UnmountItemAsync(ActiveMountInfo mount)
        {
            StatusMessage = $"{mount.MountPoint} を安全にアンマウントしています...";
            bool clean = await _mountManager.UnmountAsync(mount);
            if (!clean)
            {
                bool force = ConfirmForceKillCallback?.Invoke(mount) ?? false;
                if (force)
                {
                    await _mountManager.ForceKillAndCleanupAsync(mount);
                    StatusMessage = $"{mount.MountPoint} を強制終了・クリーンアップしました";
                }
                else
                {
                    mount.IsUnmounting = false;
                    mount.StatusText = "マウント中";
                    StatusMessage = "アンマウントを待機しています（開いているファイルを閉じて再試行してください）";
                }
            }
            else
            {
                StatusMessage = $"{mount.MountPoint} を安全にアンマウントしました";
            }
            RefreshAvailableDriveLetters();
            NotifyMountStateChanged();
        }

        public async Task LoadBrowseDirectoryAsync(string path)
        {
            if (SelectedPartition == null || !SelectedPartition.Info.IsExtFs || !HasCoreExe)
            {
                return;
            }

            string target = string.IsNullOrWhiteSpace(path) ? "/" : path;
            IsBrowseLoading = true;
            BrowseError = null;

            try
            {
                LsResponse resp = await _coreClient.ListDirectoryAsync(
                    SelectedPartition.ParentDisk.DevicePath,
                    target,
                    SelectedPartition.Info.PartitionNumber);

                CurrentBrowsePath = target;
                BrowseEntries.Clear();
                foreach (LsEntry entry in resp.Entries
                    .Where(e => e.Name != "." && e.Name != "..")
                    .OrderByDescending(e => e.IsDir)
                    .ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase))
                {
                    BrowseEntries.Add(entry);
                }
            }
            catch (Exception ex)
            {
                BrowseError = ex.Message;
                AppendLog($"[browse] {ex.Message}");
            }
            finally
            {
                IsBrowseLoading = false;
            }
        }

        public async Task OpenBrowseEntryAsync(LsEntry? entry)
        {
            if (entry == null || !entry.IsDir) return;
            string next = CombineUnixPath(CurrentBrowsePath, entry.Name);
            await LoadBrowseDirectoryAsync(next);
        }

        public MountRequest BuildCurrentMountRequest()
        {
            if (!uint.TryParse(CacheBlocksText, out uint cacheBlocks) || cacheBlocks == 0)
            {
                cacheBlocks = 2048;
            }

            return new MountRequest
            {
                SourcePath = SelectedPartition?.ParentDisk.DevicePath ?? string.Empty,
                PartitionNumber = SelectedPartition?.Info.PartitionNumber ?? 0,
                MountPoint = ActiveMountPointTarget,
                Mode = SelectedMode,
                OverlayUpperDir = OverlayUpperDir.Trim(),
                EnableDirectIo = DirectIoEditable && EnableDirectIo,
                CacheBlocks = cacheBlocks,
                RoFallbackOnDirty = RoFallbackOnDirty,
                ForceDirty = ForceDirty,
                EnableDebugLog = EnableDebugLog,
                VolumeLabel = SelectedPartition?.Info.VolumeLabel ?? string.Empty,
                AutoOpenExplorer = AutoOpenExplorer,
            };
        }

        public void ApplySuggestedOverlayPath()
        {
            if (SelectedPartition == null) return;
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrWhiteSpace(localAppData))
            {
                localAppData = AppContext.BaseDirectory;
            }
            string rawLabel = !string.IsNullOrWhiteSpace(SelectedPartition.Info.VolumeLabel)
                ? SelectedPartition.Info.VolumeLabel
                : Path.GetFileNameWithoutExtension(SelectedPartition.ParentDisk.DevicePath);
            string sanitized = string.Concat(rawLabel.Select(ch =>
                Path.GetInvalidFileNameChars().Contains(ch) ? '_' : ch));
            if (string.IsNullOrWhiteSpace(sanitized))
            {
                sanitized = "ext4";
            }
            string suffix = SelectedPartition.Info.PartitionNumber > 0
                ? $"_p{SelectedPartition.Info.PartitionNumber}"
                : string.Empty;
            OverlayUpperDir = Path.Combine(localAppData, "ZeroExt", "Overlays", $"{sanitized}{suffix}");
        }

        public void RefreshAvailableDriveLetters()
        {
            var activeLetters = ActiveMounts
                .Select(m => m.MountPoint)
                .Where(DriveLetterService.IsDriveLetter)
                .ToList();

            List<string> free = DriveLetterService.GetAvailableDriveLetters(activeLetters);
            string prev = SelectedDriveLetter;

            AvailableDriveLetters.Clear();
            foreach (string d in free)
            {
                AvailableDriveLetters.Add(d);
            }

            if (!string.IsNullOrEmpty(prev) && AvailableDriveLetters.Contains(prev))
            {
                SelectedDriveLetter = prev;
            }
            else if (AvailableDriveLetters.Count > 0)
            {
                SelectedDriveLetter = AvailableDriveLetters[0];
            }
            else
            {
                SelectedDriveLetter = string.Empty;
            }
        }

        public void AppendLog(string message)
        {
            string line = $"[{DateTime.Now:HH:mm:ss}] {message}";
            _marshalToUi(() =>
            {
                LogEntries.Add(line);
                while (LogEntries.Count > 1000)
                {
                    LogEntries.RemoveAt(0);
                }
            });
        }

        private void OnActiveMountsChanged(object? sender, NotifyCollectionChangedEventArgs e)
        {
            SyncActiveMountsToPartitions();
            RefreshAvailableDriveLetters();
            OnPropertyChanged(nameof(ActiveMountCount));
            OnPropertyChanged(nameof(ActiveMountsTabHeader));
            UnmountAllCommand.RaiseCanExecuteChanged();
            UnmountSelectedCommand.RaiseCanExecuteChanged();
            NotifyMountStateChanged();
        }

        private void SyncActiveMountsToPartitions()
        {
            foreach (PartitionItem p in DiskGroups.SelectMany(g => g.AllPartitions))
            {
                ActiveMountInfo? match = ActiveMounts.FirstOrDefault(m =>
                    string.Equals(m.SourcePath, p.ParentDisk.DevicePath, StringComparison.OrdinalIgnoreCase) &&
                    m.PartitionNumber == p.Info.PartitionNumber);
                p.ActiveMount = match;
            }
        }

        private void EnsureValidSelection()
        {
            if (SelectedPartition != null &&
                DiskGroups.SelectMany(g => g.VisiblePartitions).Contains(SelectedPartition))
            {
                return;
            }

            SelectedPartition =
                DiskGroups.SelectMany(g => g.VisiblePartitions).FirstOrDefault(p => p.Info.IsExtFs)
                ?? DiskGroups.SelectMany(g => g.VisiblePartitions).FirstOrDefault();
        }

        private void LoadPresetForSelectedPartition()
        {
            if (SelectedPartition == null) return;
            if (_settings.VolumePresets.TryGetValue(SelectedPartition.UniqueKey, out VolumePreset? preset))
            {
                _selectedMode = preset.Mode;
                _useFolderMountPoint = preset.UseFolderMountPoint;
                if (preset.UseFolderMountPoint)
                {
                    _mountFolderPath = preset.MountPoint;
                }
                else if (!string.IsNullOrEmpty(preset.MountPoint) && AvailableDriveLetters.Contains(preset.MountPoint))
                {
                    _selectedDriveLetter = preset.MountPoint;
                }
                _overlayUpperDir = preset.OverlayUpperDir;
                _enableDirectIo = preset.EnableDirectIo;
                _cacheBlocksText = (preset.CacheBlocks > 0 ? preset.CacheBlocks : 2048).ToString();
                _roFallbackOnDirty = preset.RoFallbackOnDirty;

                OnPropertyChanged(nameof(SelectedMode));
                OnPropertyChanged(nameof(IsReadOnlyMode));
                OnPropertyChanged(nameof(IsOverlayMode));
                OnPropertyChanged(nameof(IsReadWriteMode));
                OnPropertyChanged(nameof(DirectIoEditable));
                OnPropertyChanged(nameof(EffectiveDirectIo));
                OnPropertyChanged(nameof(ShowDirtyRwWarning));
                OnPropertyChanged(nameof(UseFolderMountPoint));
                OnPropertyChanged(nameof(UseDriveLetterMountPoint));
                OnPropertyChanged(nameof(SelectedDriveLetter));
                OnPropertyChanged(nameof(MountFolderPath));
                OnPropertyChanged(nameof(OverlayUpperDir));
                OnPropertyChanged(nameof(EnableDirectIo));
                OnPropertyChanged(nameof(CacheBlocksText));
                OnPropertyChanged(nameof(RoFallbackOnDirty));
            }
            else
            {
                _selectedMode = MountMode.ReadOnly;
                _enableDirectIo = true;
                _roFallbackOnDirty = true;
                _forceDirty = false;
                OnPropertyChanged(nameof(SelectedMode));
                OnPropertyChanged(nameof(IsReadOnlyMode));
                OnPropertyChanged(nameof(IsOverlayMode));
                OnPropertyChanged(nameof(IsReadWriteMode));
                OnPropertyChanged(nameof(DirectIoEditable));
                OnPropertyChanged(nameof(EffectiveDirectIo));
                OnPropertyChanged(nameof(ShowDirtyRwWarning));
                OnPropertyChanged(nameof(EnableDirectIo));
                OnPropertyChanged(nameof(RoFallbackOnDirty));
                OnPropertyChanged(nameof(ForceDirty));
            }
        }

        private void SavePresetForSelectedPartition()
        {
            if (SelectedPartition == null) return;
            if (!uint.TryParse(CacheBlocksText, out uint cb) || cb == 0) cb = 2048;

            _settings.VolumePresets[SelectedPartition.UniqueKey] = new VolumePreset
            {
                Mode = SelectedMode,
                UseFolderMountPoint = UseFolderMountPoint,
                MountPoint = ActiveMountPointTarget,
                OverlayUpperDir = OverlayUpperDir.Trim(),
                EnableDirectIo = EnableDirectIo,
                CacheBlocks = cb,
                RoFallbackOnDirty = RoFallbackOnDirty,
            };
            SaveSettingsSafe();
        }

        private void RefreshUpperDirValidation()
        {
            if (SelectedMode != MountMode.Overlay)
            {
                UpperDirCheck = new UpperDirCheckResult
                {
                    Status = UpperDirCheckStatus.None,
                    Message = string.Empty,
                    CanMount = true,
                };
                return;
            }

            UpperDirCheck = DriveLetterService.CheckOverlayUpperDir(
                OverlayUpperDir,
                SelectedPartition?.Info.ExpectedOverlayLowerId);
        }

        private void NotifyMountStateChanged()
        {
            OnPropertyChanged(nameof(ActiveMountPointTarget));
            OnPropertyChanged(nameof(CliPreviewCommand));
            OnPropertyChanged(nameof(MountButtonText));
            OnPropertyChanged(nameof(MountValidationMessage));
            OnPropertyChanged(nameof(CanMount));
            MountCommand.RaiseCanExecuteChanged();
        }

        private void SaveSettingsSafe()
        {
            try
            {
                _settingsService.Save(_settings);
            }
            catch (Exception ex)
            {
                AppendLog($"[settings] 設定保存エラー: {ex.Message}");
            }
        }

        private static string CombineUnixPath(string baseDir, string child)
        {
            string b = string.IsNullOrWhiteSpace(baseDir) ? "/" : baseDir.TrimEnd('/');
            if (string.IsNullOrEmpty(b)) b = "/";
            return b == "/" ? "/" + child : b + "/" + child;
        }

        private static string GetParentUnixPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path) || path == "/") return "/";
            string trimmed = path.TrimEnd('/');
            int slash = trimmed.LastIndexOf('/');
            return slash <= 0 ? "/" : trimmed.Substring(0, slash);
        }

        public void Dispose()
        {
            _mountManager.ActiveMounts.CollectionChanged -= OnActiveMountsChanged;
            _mountManager.Dispose();
        }
    }
}
