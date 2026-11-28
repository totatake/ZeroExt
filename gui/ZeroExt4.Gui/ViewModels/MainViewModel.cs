using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using ZeroExt4.Gui.Models;
using ZeroExt4.Gui.Services;

namespace ZeroExt4.Gui.ViewModels
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

        public string MountedBadgeText => ActiveMount != null
            ? LocalizationService.Format("Str_MountedBadgeFmt", ActiveMount.MountPoint)
            : string.Empty;

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
                ? LocalizationService.Format(
                    "Str_CapacitySummaryFmt",
                    ByteFormatter.Format(Info.UsedBytes),
                    ByteFormatter.Format(Info.EffectiveTotalBytes),
                    ByteFormatter.Format(Info.FreeBytes))
                : ByteFormatter.Format(Info.SizeBytes);

        public void RefreshLocalizedText()
        {
            OnPropertyChanged(nameof(MountedBadgeText));
            OnPropertyChanged(nameof(DisplayTitle));
            OnPropertyChanged(nameof(JournalBadgeText));
            OnPropertyChanged(nameof(CapacitySummary));
        }
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
        public string HiddenSummary => LocalizationService.Format("Str_HiddenNonExtFmt", HiddenNonExtCount);
        public bool HasNoVisibleExtPartitions => Accessible && VisiblePartitions.Count == 0;

        public void RefreshLocalizedText()
        {
            OnPropertyChanged(nameof(DisplayTitle));
            OnPropertyChanged(nameof(SubtitleText));
            OnPropertyChanged(nameof(HiddenSummary));
            foreach (PartitionItem p in AllPartitions)
            {
                p.RefreshLocalizedText();
            }
        }

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
        private readonly ZeroExt4CoreClient _coreClient;
        private readonly MountSessionManager _mountManager;
        private readonly Action<Action> _marshalToUi;
        private readonly Action<bool> _applyThemeCallback;
        private readonly Action<string> _applyLanguageCallback;
        private string? _coreExeOverride;
        private string? _coreExePath;

        private bool _isElevated;
        private bool _isWinFspAvailable = true;
        private bool _isScanning;
        private bool _isMounting;
        private bool _isRunningDoctor;
        private string _doctorSummary = string.Empty;
        private bool _showNonExtPartitions;
        private bool _isDarkTheme;
        private string _languageSetting = "auto";
        private int _selectedTabIndex;
        private string _statusMessage = LocalizationService.Get("Str_StatusReady");

        private PartitionItem? _selectedPartition;
        private MountMode _selectedMode = MountMode.ReadOnly;
        private bool _useFolderMountPoint;
        private string _selectedDriveLetter = "L:";
        private string _mountFolderPath = string.Empty;
        private string _overlayUpperDir = string.Empty;
        private UpperDirCheckResult _upperDirCheck = new UpperDirCheckResult
        {
            Status = UpperDirCheckStatus.None,
            Message = LocalizationService.Get("Str_UpperValNone"),
            CanMount = false,
        };
        private bool _enableDirectIo = true;
        private bool _enableRwFastIo;
        private bool _caseSensitive;
        private string _createUidText = string.Empty;
        private string _createGidText = string.Empty;
        private bool _inheritOwner;
        private string _createFileModeText = "0644";
        private string _createDirModeText = "0755";
        private string _cacheBlocksText = "2048";
        private bool _roFallbackOnDirty = true;
        private bool _forceDirty;
        private bool _enableDebugLog;
        private string _traceOutputPath = string.Empty;
        private bool _autoOpenExplorer = true;
        private bool _minimizeToTrayWhenMounted = true;

        private string _currentBrowsePath = "/";
        private bool _isBrowseLoading;
        private string? _browseError;
        private LsEntry? _selectedBrowseEntry;

        private bool _isOverlayBusy;
        private string _overlayDiffSummary = string.Empty;
        private string _overlayActionNotice = string.Empty;

        public ObservableCollection<DiskGroupItem> DiskGroups { get; } = new ObservableCollection<DiskGroupItem>();
        public ObservableCollection<string> AvailableDriveLetters { get; } = new ObservableCollection<string>();
        public ObservableCollection<LsEntry> BrowseEntries { get; } = new ObservableCollection<LsEntry>();
        public ObservableCollection<OverlayDiffEntry> OverlayDiffEntries { get; } = new ObservableCollection<OverlayDiffEntry>();
        public ObservableCollection<string> LogEntries { get; } = new ObservableCollection<string>();
        public ObservableCollection<ActiveMountInfo> ActiveMounts => _mountManager.ActiveMounts;

        public MountSessionManager MountManager => _mountManager;
        public ZeroExt4CoreClient CoreClient => _coreClient;

        public AsyncRelayCommand RefreshDisksCommand { get; }
        public AsyncRelayCommand MountCommand { get; }
        public AsyncRelayCommand UnmountSelectedCommand { get; }
        public AsyncRelayCommand UnmountItemCommand { get; }
        public AsyncRelayCommand UnmountAllCommand { get; }
        public AsyncRelayCommand RefreshBrowseCommand { get; }
        public AsyncRelayCommand NavigateUpBrowseCommand { get; }
        public AsyncRelayCommand RunDoctorCommand { get; }
        public AsyncRelayCommand RefreshOverlayDiffCommand { get; }
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
            Action<Action>? marshalToUi = null,
            Action<string>? applyLanguageCallback = null)
        {
            _settingsService = settingsService ?? throw new ArgumentNullException(nameof(settingsService));
            _applyThemeCallback = applyThemeCallback ?? (_ => { });
            _applyLanguageCallback = applyLanguageCallback ?? (lang => LocalizationService.ApplyLanguage(null, lang));
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
            _languageSetting = LocalizationService.NormalizeSetting(_settings.Language);
            _applyLanguageCallback(_languageSetting);
            _statusMessage = LocalizationService.Get("Str_StatusReady");
            _showNonExtPartitions = _settings.ShowNonExtPartitions;
            _autoOpenExplorer = _settings.AutoOpenExplorerOnMount;
            _minimizeToTrayWhenMounted = _settings.MinimizeToTrayWhenMounted;
            _isElevated = ElevationHelper.IsCurrentProcessElevated();
            _coreExePath = CoreExecutableLocator.Locate(_coreExeOverride, _settings.CoreExePath);

            _coreClient = new ZeroExt4CoreClient(() => _coreExePath, AppendLog);
            _mountManager = new MountSessionManager(() => _coreExePath, AppendLog, _marshalToUi);
            _mountManager.ActiveMounts.CollectionChanged += OnActiveMountsChanged;
            LocalizationService.LanguageChanged += OnLocalizationLanguageChanged;

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
            RunDoctorCommand = new AsyncRelayCommand(
                RunDoctorAsync,
                () => !IsRunningDoctor && HasCoreExe);
            RefreshOverlayDiffCommand = new AsyncRelayCommand(
                RefreshOverlayDiffAsync,
                () => CanInspectOverlay);

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
        public string CoreExeDisplay => _coreExePath ?? LocalizationService.Get("Str_CoreNotFoundDisplay");

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

        public bool IsRunningDoctor
        {
            get => _isRunningDoctor;
            private set
            {
                if (SetProperty(ref _isRunningDoctor, value))
                {
                    RunDoctorCommand.RaiseCanExecuteChanged();
                }
            }
        }

        public string DoctorSummary
        {
            get => _doctorSummary;
            private set => SetProperty(ref _doctorSummary, value);
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

        public string ThemeToggleLabel => IsDarkTheme
            ? LocalizationService.Get("Str_ThemeLight")
            : LocalizationService.Get("Str_ThemeDark");

        public string LanguageSetting
        {
            get => _languageSetting;
            set
            {
                string norm = LocalizationService.NormalizeSetting(value);
                if (SetProperty(ref _languageSetting, norm))
                {
                    _settings.Language = norm;
                    SaveSettingsSafe();
                    _applyLanguageCallback(norm);
                }
            }
        }

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
                    OnPropertyChanged(nameof(IsReadWriteBlockedByHealth));
                    OnPropertyChanged(nameof(CanSelectReadWrite));
                    OnPropertyChanged(nameof(ReadWriteHealthNotice));
                    OnPropertyChanged(nameof(ShowDirtyRwWarning));
                    OnPropertyChanged(nameof(SelectedVolumeTitle));
                    OnPropertyChanged(nameof(SelectedVolumeSubtitle));
                    OnPropertyChanged(nameof(SelectedJournalHeaderText));
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
        public bool IsReadWriteBlockedByHealth =>
            SelectedPartition?.Info.IsExtFs == true &&
            (SelectedPartition.Info.IsDirty ||
             (SelectedPartition.Info.MountAdvice != null && !SelectedPartition.Info.MountAdvice.ReadWriteAllowed));
        public bool CanSelectReadWrite => !IsReadWriteBlockedByHealth;
        public string ReadWriteHealthNotice
        {
            get
            {
                if (!IsReadWriteBlockedByHealth) return string.Empty;
                string? reason = SelectedPartition?.Info.MountAdvice?.RwReason;
                if (!string.IsNullOrWhiteSpace(reason))
                {
                    return reason;
                }
                return LocalizationService.Get("Str_ValRwHealthBlocked");
            }
        }

        public string SelectedVolumeTitle =>
            SelectedPartition == null
                ? LocalizationService.Get("Str_SelectVolumePrompt")
                : $"{SelectedPartition.DisplayTitle}  ({SelectedPartition.FsBadgeText})";

        public string SelectedVolumeSubtitle =>
            SelectedPartition == null
                ? LocalizationService.Get("Str_SelectVolumeSubPrompt")
                : $"{SelectedPartition.ParentDisk.DevicePath}  |  {SelectedPartition.PartitionSubtitle}  |  UUID: {(string.IsNullOrWhiteSpace(SelectedPartition.Info.Uuid) ? "-" : SelectedPartition.Info.Uuid)}";

        public string SelectedJournalHeaderText =>
            LocalizationService.Format("Str_JournalStateFmt", SelectedPartition?.Info.JournalDisplayText ?? "-");

        public string ExpectedLowerIdDisplay =>
            SelectedPartition?.Info.GetExpectedOverlayLowerIdV2(CaseSensitive)
            ?? SelectedPartition?.Info.ExpectedOverlayLowerId
            ?? "-";

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
                    OnPropertyChanged(nameof(IsWriteCapableMode));
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
        public bool IsWriteCapableMode => SelectedMode == MountMode.ReadWrite || SelectedMode == MountMode.Overlay;
        public bool IsExplicitUidGidEnabled => !InheritOwner;

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
                    ClearOverlayPreviewState();
                    RefreshUpperDirValidation();
                    NotifyOverlayActionsChanged();
                    NotifyMountStateChanged();
                }
            }
        }

        public UpperDirCheckResult UpperDirCheck
        {
            get => _upperDirCheck;
            private set
            {
                if (SetProperty(ref _upperDirCheck, value))
                {
                    NotifyOverlayActionsChanged();
                }
            }
        }

        public bool IsOverlayBusy
        {
            get => _isOverlayBusy;
            private set
            {
                if (SetProperty(ref _isOverlayBusy, value))
                {
                    NotifyOverlayActionsChanged();
                }
            }
        }

        public string OverlayDiffSummary
        {
            get => _overlayDiffSummary;
            private set
            {
                if (SetProperty(ref _overlayDiffSummary, value))
                {
                    OnPropertyChanged(nameof(HasOverlayDiffSummary));
                }
            }
        }

        public bool HasOverlayDiffSummary => !string.IsNullOrWhiteSpace(OverlayDiffSummary);

        public string OverlayActionNotice
        {
            get => _overlayActionNotice;
            private set
            {
                if (SetProperty(ref _overlayActionNotice, value))
                {
                    OnPropertyChanged(nameof(HasOverlayActionNotice));
                }
            }
        }

        public bool HasOverlayActionNotice => !string.IsNullOrWhiteSpace(OverlayActionNotice);

        public bool HasOverlayDiffEntries => OverlayDiffEntries.Count > 0;

        public bool CanInspectOverlay =>
            !IsOverlayBusy && HasCoreExe && !string.IsNullOrWhiteSpace(OverlayUpperDir) && Directory.Exists(OverlayUpperDir);

        public bool CanExportOverlay => CanInspectOverlay;

        public bool CanResetOverlay =>
            CanInspectOverlay &&
            UpperDirCheck.CanMount &&
            UpperDirCheck.Status != UpperDirCheckStatus.WillCreate &&
            !IsUpperDirActivelyMounted();

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

        public bool EnableRwFastIo
        {
            get => _enableRwFastIo;
            set
            {
                if (SetProperty(ref _enableRwFastIo, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool CaseSensitive
        {
            get => _caseSensitive;
            set
            {
                if (SetProperty(ref _caseSensitive, value))
                {
                    OnPropertyChanged(nameof(ExpectedLowerIdDisplay));
                    RefreshUpperDirValidation();
                    NotifyMountStateChanged();
                }
            }
        }

        public string CreateUidText
        {
            get => _createUidText;
            set
            {
                if (SetProperty(ref _createUidText, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public string CreateGidText
        {
            get => _createGidText;
            set
            {
                if (SetProperty(ref _createGidText, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public bool InheritOwner
        {
            get => _inheritOwner;
            set
            {
                if (SetProperty(ref _inheritOwner, value))
                {
                    OnPropertyChanged(nameof(IsExplicitUidGidEnabled));
                    NotifyMountStateChanged();
                }
            }
        }

        public string CreateFileModeText
        {
            get => _createFileModeText;
            set
            {
                if (SetProperty(ref _createFileModeText, value))
                {
                    NotifyMountStateChanged();
                }
            }
        }

        public string CreateDirModeText
        {
            get => _createDirModeText;
            set
            {
                if (SetProperty(ref _createDirModeText, value))
                {
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

        public string TraceOutputPath
        {
            get => _traceOutputPath;
            set
            {
                if (SetProperty(ref _traceOutputPath, value))
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
                    return "zeroext4.exe mount --source <source> --mountpoint L:";
                }
                return ZeroExt4ArgBuilder.BuildMountPreviewCommand(BuildCurrentMountRequest());
            }
        }

        public string MountButtonText =>
            IsMounting
                ? LocalizationService.Get("Str_MountBtnMounting")
                : SelectedMode switch
                {
                    MountMode.Overlay => LocalizationService.Get("Str_MountBtnOverlay"),
                    MountMode.ReadWrite => LocalizationService.Get("Str_MountBtnRw"),
                    _ => LocalizationService.Get("Str_MountBtnRo"),
                };

        public string MountValidationMessage
        {
            get
            {
                if (!HasCoreExe) return LocalizationService.Get("Str_ValNoCoreExe");
                if (!IsWinFspAvailable) return LocalizationService.Get("Str_ValNoWinFsp");
                if (SelectedPartition == null) return LocalizationService.Get("Str_ValNoPartition");
                if (!SelectedPartition.Info.IsExtFs) return LocalizationService.Get("Str_ValNotExtFs");
                if (SelectedPartition.IsMounted)
                {
                    return LocalizationService.Format("Str_ValAlreadyMountedFmt", SelectedPartition.ActiveMount?.MountPoint);
                }

                if (UseFolderMountPoint)
                {
                    var (valid, msg) = DriveLetterService.ValidateMountFolder(MountFolderPath);
                    if (!valid) return msg;
                }
                else
                {
                    if (string.IsNullOrWhiteSpace(SelectedDriveLetter))
                    {
                        return LocalizationService.Get("Str_ValNoDriveLetters");
                    }
                    if (ActiveMounts.Any(m => string.Equals(
                        ZeroExt4ArgBuilder.NormalizeMountPointArg(m.MountPoint),
                        ZeroExt4ArgBuilder.NormalizeMountPointArg(SelectedDriveLetter),
                        StringComparison.OrdinalIgnoreCase)))
                    {
                        return LocalizationService.Format("Str_ValDriveInUseFmt", SelectedDriveLetter);
                    }
                }

                if (SelectedMode == MountMode.Overlay && !UpperDirCheck.CanMount)
                {
                    return UpperDirCheck.Message;
                }

                if (SelectedMode == MountMode.ReadWrite && IsReadWriteBlockedByHealth && !ForceDirty)
                {
                    return LocalizationService.Get("Str_ValRwHealthBlocked");
                }

                if (!uint.TryParse(CacheBlocksText, out uint cb) || cb == 0)
                {
                    return LocalizationService.Get("Str_ValInvalidCacheBlocks");
                }

                if (IsWriteCapableMode)
                {
                    if (!InheritOwner)
                    {
                        if (!string.IsNullOrWhiteSpace(CreateUidText) && !uint.TryParse(CreateUidText.Trim(), out _))
                        {
                            return LocalizationService.Get("Str_ValInvalidUidGid");
                        }
                        if (!string.IsNullOrWhiteSpace(CreateGidText) && !uint.TryParse(CreateGidText.Trim(), out _))
                        {
                            return LocalizationService.Get("Str_ValInvalidUidGid");
                        }
                    }
                    if (!string.IsNullOrWhiteSpace(CreateFileModeText) &&
                        !ZeroExt4ArgBuilder.TryParseOctalPermission(CreateFileModeText, out _))
                    {
                        return LocalizationService.Get("Str_ValInvalidPosixMode");
                    }
                    if (!string.IsNullOrWhiteSpace(CreateDirModeText) &&
                        !ZeroExt4ArgBuilder.TryParseOctalPermission(CreateDirModeText, out _))
                    {
                        return LocalizationService.Get("Str_ValInvalidPosixMode");
                    }
                }

                return string.Empty;
            }
        }

        public bool CanMount => string.IsNullOrEmpty(MountValidationMessage);

        public int ActiveMountCount => ActiveMounts.Count;
        public string ActiveMountCountText => LocalizationService.Format("Str_StatusBarActiveMountsFmt", ActiveMounts.Count);
        public string ActiveMountsTabHeader => ActiveMounts.Count > 0
            ? LocalizationService.Format("Str_TabActiveMountsCountFmt", ActiveMounts.Count)
            : LocalizationService.Get("Str_TabActiveMounts");

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
            RunDoctorCommand.RaiseCanExecuteChanged();
        }

        public async Task RefreshAllAsync()
        {
            if (!HasCoreExe)
            {
                StatusMessage = LocalizationService.Get("Str_StatusCoreMissing");
                return;
            }

            IsScanning = true;
            StatusMessage = LocalizationService.Get("Str_StatusScanning");
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
                        AppendLog(LocalizationService.Format("Str_LogScanMissingImgFmt", imgPath));
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
                        AppendLog(LocalizationService.Format("Str_LogScanImgFailFmt", imgPath, ex.Message));
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
                StatusMessage = LocalizationService.Format("Str_StatusScanDoneFmt", DiskGroups.Count, extCount);
            }
            catch (Exception ex)
            {
                AppendLog(LocalizationService.Format("Str_LogScanErrFmt", ex.Message));
                StatusMessage = LocalizationService.Format("Str_StatusScanErrFmt", ex.Message);
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
                StatusMessage = LocalizationService.Format("Str_StatusLoadingImgFmt", Path.GetFileName(fullPath));
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

                StatusMessage = LocalizationService.Format("Str_StatusLoadedImgFmt", Path.GetFileName(fullPath));
            }
            catch (Exception ex)
            {
                AppendLog(LocalizationService.Format("Str_LogImgErrFmt", fullPath, ex.Message));
                ShowErrorNotification?.Invoke(
                    LocalizationService.Get("Str_ErrInspectImgTitle"),
                    ex.Message,
                    LocalizationService.Get("Str_ErrInspectImgDetail"));
                StatusMessage = LocalizationService.Get("Str_StatusLoadImgFailed");
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
            if (SelectedPartition == null) return;
            RefreshUpperDirValidation();
            NotifyMountStateChanged();
            if (!CanMount) return;

            MountRequest req = BuildCurrentMountRequest();
            SavePresetForSelectedPartition();

            IsMounting = true;
            StatusMessage = LocalizationService.Format("Str_StatusMountingFmt", req.MountPoint);
            try
            {
                ActiveMountInfo info = await _mountManager.StartMountAsync(req);
                RefreshAvailableDriveLetters();
                StatusMessage = LocalizationService.Format("Str_StatusMountedFmt", info.MountPoint, info.ModeDisplay);

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
                    mf.Translated.Message + "\n\n" + LocalizationService.Get("Str_SuggestionPrefix") + mf.Translated.Suggestion,
                    mf.Translated.RawDetail);
            }
            catch (Exception ex)
            {
                StatusMessage = LocalizationService.Get("Str_StatusMountFailed");
                ShowErrorNotification?.Invoke(LocalizationService.Get("Str_ErrMountTitle"), ex.Message, null);
            }
            finally
            {
                IsMounting = false;
                NotifyMountStateChanged();
            }
        }

        public async Task RunDoctorAsync()
        {
            if (!HasCoreExe || IsRunningDoctor) return;
            IsRunningDoctor = true;
            StatusMessage = LocalizationService.Get("Str_DoctorRunning");
            try
            {
                string? src = SelectedPartition?.ParentDisk.DevicePath;
                uint? part = SelectedPartition?.Info.PartitionNumber;
                string? ovDir = SelectedMode == MountMode.Overlay && !string.IsNullOrWhiteSpace(OverlayUpperDir)
                    ? OverlayUpperDir.Trim()
                    : null;

                DoctorResponse doc = await _coreClient.RunDoctorAsync(src, part, ovDir, CaseSensitive);
                IsElevated = doc.IsElevated;
                IsWinFspAvailable = doc.WinFsp.Available;
                OnPropertyChanged(nameof(NonElevatedNoticeVisible));

                var lines = new List<string>
                {
                    $"status={doc.OverallStatus} | elevated={doc.IsElevated} | can_mount_drive={doc.CanMountDrive}",
                    $"winfsp={(doc.WinFsp.Available ? $"OK ({doc.WinFsp.Version})" : "NOT FOUND")}",
                };
                if (doc.Source != null)
                {
                    if (!doc.Source.Readable || !doc.Source.PartitionFound)
                    {
                        lines.Add($"source=ERROR ({doc.Source.Error})");
                    }
                    else if (doc.Source.Partition != null)
                    {
                        PartitionInfo p = doc.Source.Partition;
                        lines.Add($"source={p.FsKind} | journal={p.JournalState} | csum={p.ChecksumSummary} | rec={p.MountAdvice?.RecommendedMode ?? "-"}");
                    }
                }
                if (doc.Overlay != null)
                {
                    lines.Add($"overlay={doc.Overlay.Status} (can_mount={doc.Overlay.CanMount}, marker_ver={doc.Overlay.MarkerVersion})");
                }
                DoctorSummary = string.Join(Environment.NewLine, lines);
                AppendLog("[doctor] " + string.Join(" ; ", lines));
                StatusMessage = LocalizationService.Format("Str_StatusDoctorDoneFmt", doc.OverallStatus);
            }
            catch (Exception ex)
            {
                DoctorSummary = ex.Message;
                AppendLog($"[doctor] {ex.Message}");
                StatusMessage = LocalizationService.Format("Str_StatusDoctorErrFmt", ex.Message);
            }
            finally
            {
                IsRunningDoctor = false;
            }
        }

        public async Task UnmountItemAsync(ActiveMountInfo mount)
        {
            StatusMessage = LocalizationService.Format("Str_StatusUnmountingFmt", mount.MountPoint);
            bool clean = await _mountManager.UnmountAsync(mount);
            if (!clean)
            {
                bool force = ConfirmForceKillCallback?.Invoke(mount) ?? false;
                if (force)
                {
                    await _mountManager.ForceKillAndCleanupAsync(mount);
                    StatusMessage = LocalizationService.Format("Str_StatusForceKilledFmt", mount.MountPoint);
                }
                else
                {
                    mount.IsUnmounting = false;
                    mount.StatusText = LocalizationService.Get("Str_MountStatusActive");
                    StatusMessage = LocalizationService.Get("Str_StatusUnmountWaiting");
                }
            }
            else
            {
                StatusMessage = LocalizationService.Format("Str_StatusUnmountedFmt", mount.MountPoint);
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
            uint? parsedUid = !string.IsNullOrWhiteSpace(CreateUidText) && uint.TryParse(CreateUidText.Trim(), out uint u)
                ? u
                : null;
            uint? parsedGid = !string.IsNullOrWhiteSpace(CreateGidText) && uint.TryParse(CreateGidText.Trim(), out uint g)
                ? g
                : null;

            return new MountRequest
            {
                SourcePath = SelectedPartition?.ParentDisk.DevicePath ?? string.Empty,
                PartitionNumber = SelectedPartition?.Info.PartitionNumber ?? 0,
                MountPoint = ActiveMountPointTarget,
                Mode = SelectedMode,
                OverlayUpperDir = OverlayUpperDir.Trim(),
                EnableDirectIo = DirectIoEditable && EnableDirectIo,
                EnableRwFastIo = SelectedMode == MountMode.ReadWrite && EnableRwFastIo,
                CaseSensitive = CaseSensitive,
                CreateUid = parsedUid,
                CreateGid = parsedGid,
                InheritOwner = InheritOwner,
                CreateFileModeOctal = string.IsNullOrWhiteSpace(CreateFileModeText) ? "0644" : CreateFileModeText.Trim(),
                CreateDirModeOctal = string.IsNullOrWhiteSpace(CreateDirModeText) ? "0755" : CreateDirModeText.Trim(),
                CacheBlocks = cacheBlocks,
                RoFallbackOnDirty = RoFallbackOnDirty,
                ForceDirty = ForceDirty,
                EnableDebugLog = EnableDebugLog,
                TraceOutputPath = TraceOutputPath.Trim(),
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
            string csSuffix = CaseSensitive ? "_cs" : string.Empty;
            OverlayUpperDir = Path.Combine(localAppData, "ZeroExt4", "Overlays", $"{sanitized}{suffix}{csSuffix}");
        }

        public async Task RefreshOverlayDiffAsync()
        {
            if (!CanInspectOverlay) return;
            IsOverlayBusy = true;
            OverlayActionNotice = string.Empty;
            try
            {
                MountRequest curReq = BuildCurrentMountRequest();
                OverlayStatusResponse resp = await _coreClient.GetOverlayDiffAsync(
                    OverlayUpperDir.Trim(),
                    SelectedPartition?.ParentDisk.DevicePath,
                    SelectedPartition?.Info.PartitionNumber,
                    CaseSensitive,
                    diff: true,
                    createUid: curReq.CreateUid,
                    createGid: curReq.CreateGid,
                    inheritOwner: curReq.InheritOwner,
                    fmodeOctal: curReq.CreateFileModeOctal,
                    dmodeOctal: curReq.CreateDirModeOctal);

                _marshalToUi(() =>
                {
                    OverlayDiffEntries.Clear();
                    foreach (OverlayDiffEntry item in resp.Entries)
                    {
                        OverlayDiffEntries.Add(item);
                    }
                    OnPropertyChanged(nameof(HasOverlayDiffEntries));
                    OverlayDiffSummary = LocalizationService.Format(
                        "Str_OverlayDiffSummaryFmt",
                        resp.Summary.AddedCount,
                        resp.Summary.ModifiedCount,
                        resp.Summary.DeletedCount,
                        resp.Summary.OpaqueDirCount,
                        ByteFormatter.Format(resp.Summary.TotalUpperBytes));
                    if (resp.Entries.Count == 0)
                    {
                        OverlayActionNotice = LocalizationService.Get("Str_OverlayDiffEmptyNote");
                    }
                });
            }
            catch (Exception ex)
            {
                AppendLog($"[overlay] {ex.Message}");
                OverlayActionNotice = ex.Message;
            }
            finally
            {
                IsOverlayBusy = false;
            }
        }

        public async Task ExportOverlayAsync(string outputTarPath)
        {
            if (!CanExportOverlay || string.IsNullOrWhiteSpace(outputTarPath)) return;
            IsOverlayBusy = true;
            OverlayActionNotice = string.Empty;
            try
            {
                MountRequest curReq = BuildCurrentMountRequest();
                OverlayExportResponse resp = await _coreClient.ExportOverlayAsync(
                    OverlayUpperDir.Trim(),
                    outputTarPath,
                    SelectedPartition?.ParentDisk.DevicePath,
                    SelectedPartition?.Info.PartitionNumber,
                    CaseSensitive,
                    createUid: curReq.CreateUid,
                    createGid: curReq.CreateGid,
                    inheritOwner: curReq.InheritOwner,
                    fmodeOctal: curReq.CreateFileModeOctal,
                    dmodeOctal: curReq.CreateDirModeOctal);

                string notice = LocalizationService.Format(
                    "Str_OverlayExportDoneFmt",
                    resp.ExportedEntries,
                    ByteFormatter.Format(resp.ArchiveBytes),
                    resp.OutputPath);
                OverlayActionNotice = notice;
                StatusMessage = notice;
                AppendLog($"[overlay] {notice}");
            }
            catch (Exception ex)
            {
                AppendLog($"[overlay] {ex.Message}");
                OverlayActionNotice = ex.Message;
            }
            finally
            {
                IsOverlayBusy = false;
            }
        }

        public async Task ResetOverlayAsync()
        {
            if (!HasCoreExe || IsOverlayBusy || string.IsNullOrWhiteSpace(OverlayUpperDir)) return;
            if (IsUpperDirActivelyMounted())
            {
                OverlayActionNotice = LocalizationService.Get("Str_OverlayResetBlockedMounted");
                return;
            }

            IsOverlayBusy = true;
            OverlayActionNotice = string.Empty;
            try
            {
                OverlayResetResponse resp = await _coreClient.ResetOverlayAsync(OverlayUpperDir.Trim());
                string notice = LocalizationService.Format("Str_OverlayResetDoneFmt", resp.RemovedEntries);
                _marshalToUi(() =>
                {
                    OverlayDiffEntries.Clear();
                    OnPropertyChanged(nameof(HasOverlayDiffEntries));
                    OverlayDiffSummary = string.Empty;
                    OverlayActionNotice = notice;
                    StatusMessage = notice;
                });
                AppendLog($"[overlay] {notice}");
                RefreshUpperDirValidation();
            }
            catch (Exception ex)
            {
                AppendLog($"[overlay] {ex.Message}");
                OverlayActionNotice = ex.Message;
            }
            finally
            {
                IsOverlayBusy = false;
            }
        }

        private bool IsUpperDirActivelyMounted()
        {
            if (string.IsNullOrWhiteSpace(OverlayUpperDir)) return false;
            string norm;
            try
            {
                norm = Path.GetFullPath(OverlayUpperDir.Trim()).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            }
            catch
            {
                norm = OverlayUpperDir.Trim();
            }
            return ActiveMounts.Any(m =>
                !string.IsNullOrWhiteSpace(m.OverlayUpperDir) &&
                string.Equals(
                    Path.GetFullPath(m.OverlayUpperDir.Trim()).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                    norm,
                    StringComparison.OrdinalIgnoreCase));
        }

        private void ClearOverlayPreviewState()
        {
            if (OverlayDiffEntries.Count > 0)
            {
                OverlayDiffEntries.Clear();
                OnPropertyChanged(nameof(HasOverlayDiffEntries));
            }
            OverlayDiffSummary = string.Empty;
            OverlayActionNotice = string.Empty;
        }

        private void NotifyOverlayActionsChanged()
        {
            OnPropertyChanged(nameof(CanInspectOverlay));
            OnPropertyChanged(nameof(CanExportOverlay));
            OnPropertyChanged(nameof(CanResetOverlay));
            RefreshOverlayDiffCommand?.RaiseCanExecuteChanged();
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
            OnPropertyChanged(nameof(ActiveMountCountText));
            OnPropertyChanged(nameof(ActiveMountsTabHeader));
            UnmountAllCommand.RaiseCanExecuteChanged();
            UnmountSelectedCommand.RaiseCanExecuteChanged();
            NotifyOverlayActionsChanged();
            NotifyMountStateChanged();
        }

        private void OnLocalizationLanguageChanged(object? sender, EventArgs e)
        {
            _marshalToUi(() =>
            {
                OnPropertyChanged(nameof(ThemeToggleLabel));
                OnPropertyChanged(nameof(CoreExeDisplay));
                OnPropertyChanged(nameof(SelectedVolumeTitle));
                OnPropertyChanged(nameof(SelectedVolumeSubtitle));
                OnPropertyChanged(nameof(SelectedJournalHeaderText));
                OnPropertyChanged(nameof(ExpectedLowerIdDisplay));
                OnPropertyChanged(nameof(ReadWriteHealthNotice));
                OnPropertyChanged(nameof(ActiveMountCountText));
                OnPropertyChanged(nameof(ActiveMountsTabHeader));
                foreach (DiskGroupItem g in DiskGroups)
                {
                    g.RefreshLocalizedText();
                }
                foreach (ActiveMountInfo m in ActiveMounts)
                {
                    if (!m.IsUnmounting)
                    {
                        m.StatusText = m.FellBackToRo
                            ? LocalizationService.Get("Str_MountStatusFallbackRo")
                            : LocalizationService.Get("Str_MountStatusActive");
                    }
                    else
                    {
                        m.StatusText = LocalizationService.Get("Str_MountStatusUnmounting");
                    }
                }
                RefreshUpperDirValidation();
                NotifyMountStateChanged();
            });
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
                _enableRwFastIo = preset.EnableRwFastIo;
                _caseSensitive = preset.CaseSensitive;
                _createUidText = preset.CreateUid.HasValue ? preset.CreateUid.Value.ToString() : string.Empty;
                _createGidText = preset.CreateGid.HasValue ? preset.CreateGid.Value.ToString() : string.Empty;
                _inheritOwner = preset.InheritOwner;
                _createFileModeText = string.IsNullOrWhiteSpace(preset.CreateFileModeOctal) ? "0644" : preset.CreateFileModeOctal;
                _createDirModeText = string.IsNullOrWhiteSpace(preset.CreateDirModeOctal) ? "0755" : preset.CreateDirModeOctal;
                _cacheBlocksText = (preset.CacheBlocks > 0 ? preset.CacheBlocks : 2048).ToString();
                _roFallbackOnDirty = preset.RoFallbackOnDirty;
                if (IsReadWriteBlockedByHealth && _selectedMode == MountMode.ReadWrite)
                {
                    _selectedMode = MountMode.ReadOnly;
                    _forceDirty = false;
                }

                OnPropertyChanged(nameof(SelectedMode));
                OnPropertyChanged(nameof(IsReadOnlyMode));
                OnPropertyChanged(nameof(IsOverlayMode));
                OnPropertyChanged(nameof(IsReadWriteMode));
                OnPropertyChanged(nameof(IsWriteCapableMode));
                OnPropertyChanged(nameof(DirectIoEditable));
                OnPropertyChanged(nameof(EffectiveDirectIo));
                OnPropertyChanged(nameof(ShowDirtyRwWarning));
                OnPropertyChanged(nameof(UseFolderMountPoint));
                OnPropertyChanged(nameof(UseDriveLetterMountPoint));
                OnPropertyChanged(nameof(SelectedDriveLetter));
                OnPropertyChanged(nameof(MountFolderPath));
                OnPropertyChanged(nameof(OverlayUpperDir));
                OnPropertyChanged(nameof(EnableDirectIo));
                OnPropertyChanged(nameof(EnableRwFastIo));
                OnPropertyChanged(nameof(CaseSensitive));
                OnPropertyChanged(nameof(CreateUidText));
                OnPropertyChanged(nameof(CreateGidText));
                OnPropertyChanged(nameof(InheritOwner));
                OnPropertyChanged(nameof(IsExplicitUidGidEnabled));
                OnPropertyChanged(nameof(CreateFileModeText));
                OnPropertyChanged(nameof(CreateDirModeText));
                OnPropertyChanged(nameof(CacheBlocksText));
                OnPropertyChanged(nameof(RoFallbackOnDirty));
            }
            else
            {
                _selectedMode = MountMode.ReadOnly;
                _enableDirectIo = true;
                _enableRwFastIo = false;
                _caseSensitive = false;
                _createUidText = string.Empty;
                _createGidText = string.Empty;
                _inheritOwner = false;
                _createFileModeText = "0644";
                _createDirModeText = "0755";
                _roFallbackOnDirty = true;
                _forceDirty = false;
                OnPropertyChanged(nameof(SelectedMode));
                OnPropertyChanged(nameof(IsReadOnlyMode));
                OnPropertyChanged(nameof(IsOverlayMode));
                OnPropertyChanged(nameof(IsReadWriteMode));
                OnPropertyChanged(nameof(IsWriteCapableMode));
                OnPropertyChanged(nameof(DirectIoEditable));
                OnPropertyChanged(nameof(EffectiveDirectIo));
                OnPropertyChanged(nameof(ShowDirtyRwWarning));
                OnPropertyChanged(nameof(EnableDirectIo));
                OnPropertyChanged(nameof(EnableRwFastIo));
                OnPropertyChanged(nameof(CaseSensitive));
                OnPropertyChanged(nameof(CreateUidText));
                OnPropertyChanged(nameof(CreateGidText));
                OnPropertyChanged(nameof(InheritOwner));
                OnPropertyChanged(nameof(IsExplicitUidGidEnabled));
                OnPropertyChanged(nameof(CreateFileModeText));
                OnPropertyChanged(nameof(CreateDirModeText));
                OnPropertyChanged(nameof(RoFallbackOnDirty));
                OnPropertyChanged(nameof(ForceDirty));
            }
        }

        private void SavePresetForSelectedPartition()
        {
            if (SelectedPartition == null) return;
            if (!uint.TryParse(CacheBlocksText, out uint cb) || cb == 0) cb = 2048;
            uint? parsedUid = !string.IsNullOrWhiteSpace(CreateUidText) && uint.TryParse(CreateUidText.Trim(), out uint u)
                ? u
                : null;
            uint? parsedGid = !string.IsNullOrWhiteSpace(CreateGidText) && uint.TryParse(CreateGidText.Trim(), out uint g)
                ? g
                : null;

            _settings.VolumePresets[SelectedPartition.UniqueKey] = new VolumePreset
            {
                Mode = SelectedMode,
                UseFolderMountPoint = UseFolderMountPoint,
                MountPoint = ActiveMountPointTarget,
                OverlayUpperDir = OverlayUpperDir.Trim(),
                EnableDirectIo = EnableDirectIo,
                EnableRwFastIo = EnableRwFastIo,
                CaseSensitive = CaseSensitive,
                CreateUid = parsedUid,
                CreateGid = parsedGid,
                InheritOwner = InheritOwner,
                CreateFileModeOctal = string.IsNullOrWhiteSpace(CreateFileModeText) ? "0644" : CreateFileModeText.Trim(),
                CreateDirModeOctal = string.IsNullOrWhiteSpace(CreateDirModeText) ? "0755" : CreateDirModeText.Trim(),
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
                SelectedPartition?.Info,
                CaseSensitive);
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
                AppendLog(LocalizationService.Format("Str_LogSettingsSaveErrFmt", ex.Message));
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
            LocalizationService.LanguageChanged -= OnLocalizationLanguageChanged;
            _mountManager.ActiveMounts.CollectionChanged -= OnActiveMountsChanged;
            _mountManager.Dispose();
        }
    }
}
