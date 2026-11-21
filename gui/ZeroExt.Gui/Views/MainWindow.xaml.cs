using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using Microsoft.Win32;
using ZeroExt.Gui.Models;
using ZeroExt.Gui.Services;
using ZeroExt.Gui.ViewModels;

namespace ZeroExt.Gui.Views
{
    public partial class MainWindow : Window
    {
        private const uint WM_DROPFILES = 0x0233;
        private const uint WM_COPYDATA = 0x004A;
        private const uint WM_COPYGLOBALDATA = 0x0049;
        private const uint MSGFLT_ALLOW = 1;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool ChangeWindowMessageFilterEx(
            IntPtr hWnd, uint msg, uint action, IntPtr pChangeFilterStruct);

        private readonly MainViewModel _vm;
        private readonly Action _notifyMinimizedToTray;
        private bool _allowClose;

        public MainWindow(MainViewModel viewModel, Action? notifyMinimizedToTray = null)
        {
            _vm = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
            _notifyMinimizedToTray = notifyMinimizedToTray ?? (() => { });
            DataContext = _vm;
            InitializeComponent();

            _vm.ShowErrorNotification = (title, message, detail) =>
            {
                string full = string.IsNullOrWhiteSpace(detail)
                    ? message
                    : $"{message}\n\n--- {LocalizationService.Get("Str_DetailHeader")} ---\n{detail}";
                MessageBox.Show(this, full, title, MessageBoxButton.OK, MessageBoxImage.Warning);
            };

            _vm.ConfirmForceKillCallback = mount =>
            {
                MessageBoxResult r = MessageBox.Show(
                    this,
                    LocalizationService.Format("Str_ForceKillMsgFmt", mount.MountPoint),
                    LocalizationService.Get("Str_ForceKillTitle"),
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Warning);
                return r == MessageBoxResult.Yes;
            };
        }

        public void ForceCloseWithoutTray()
        {
            _allowClose = true;
            Close();
        }

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);
            if (_vm.IsElevated)
            {
                // Allow drag-and-drop from non-elevated Explorer into elevated ZeroExtGui
                IntPtr hwnd = new WindowInteropHelper(this).Handle;
                if (hwnd != IntPtr.Zero)
                {
                    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, IntPtr.Zero);
                    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, IntPtr.Zero);
                    ChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, IntPtr.Zero);
                }
            }
        }

        private void OnWindowDragOver(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
            {
                e.Effects = DragDropEffects.Copy;
            }
            else
            {
                e.Effects = DragDropEffects.None;
            }
            e.Handled = true;
        }

        private async void OnWindowDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            if (e.Data.GetData(DataFormats.FileDrop) is string[] files)
            {
                foreach (string f in files)
                {
                    if (File.Exists(f))
                    {
                        await _vm.AddImageFileAsync(f, autoSelect: true);
                    }
                }
            }
        }

        private void OnPartitionCardClick(object sender, MouseButtonEventArgs e)
        {
            if (sender is FrameworkElement fe && fe.DataContext is PartitionItem item)
            {
                _vm.SelectedPartition = item;
            }
        }

        private async void OnBrowseEntryDoubleClick(object sender, MouseButtonEventArgs e)
        {
            if (_vm.SelectedBrowseEntry != null && _vm.SelectedBrowseEntry.IsDir)
            {
                await _vm.OpenBrowseEntryAsync(_vm.SelectedBrowseEntry);
            }
        }

        private async void OnAddImageClick(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog
            {
                Title = LocalizationService.Get("Str_OpenImgDialogTitle"),
                Filter = LocalizationService.Get("Str_OpenImgDialogFilter"),
                Multiselect = true,
            };
            if (dlg.ShowDialog(this) == true)
            {
                foreach (string file in dlg.FileNames)
                {
                    await _vm.AddImageFileAsync(file, autoSelect: true);
                }
            }
        }

        private async void OnCreateImageClick(object sender, RoutedEventArgs e)
        {
            if (!_vm.HasCoreExe)
            {
                MessageBox.Show(
                    this,
                    LocalizationService.Get("Str_ErrCoreMissingCreate"),
                    LocalizationService.Get("Str_ErrorTitle"),
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }
            var dlg = new CreateImageDialog(_vm.CoreClient) { Owner = this };
            if (dlg.ShowDialog() == true && !string.IsNullOrEmpty(dlg.CreatedImagePath))
            {
                await _vm.AddImageFileAsync(dlg.CreatedImagePath, autoSelect: true);
            }
        }

        private void OnForceUnmountClick(object sender, RoutedEventArgs e)
        {
            if (!_vm.HasCoreExe)
            {
                MessageBox.Show(
                    this,
                    LocalizationService.Get("Str_ErrCoreMissingExec"),
                    LocalizationService.Get("Str_ErrorTitle"),
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }
            var dlg = new ForceUnmountDialog(_vm.CoreClient) { Owner = this };
            dlg.ShowDialog();
            _vm.RefreshAvailableDriveLetters();
        }

        private void OnOpenWinFspGuideClick(object sender, RoutedEventArgs e)
        {
            var dlg = new WinFspGuideDialog(
                _vm.IsWinFspAvailable,
                async () =>
                {
                    await _vm.RefreshAllAsync();
                    return _vm.IsWinFspAvailable;
                })
            {
                Owner = this,
            };
            dlg.ShowDialog();
        }

        private async void OnElevateClick(object sender, RoutedEventArgs e)
        {
            if (_vm.ActiveMountCount > 0)
            {
                MessageBoxResult res = MessageBox.Show(
                    this,
                    LocalizationService.Format("Str_ElevateActiveMsgFmt", _vm.ActiveMountCount),
                    LocalizationService.Get("Str_ElevateActiveTitle"),
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Question);
                if (res != MessageBoxResult.Yes) return;
                await _vm.MountManager.UnmountAllAsync();
            }

            if (ElevationHelper.RestartElevated())
            {
                _allowClose = true;
                Application.Current.Shutdown();
            }
        }

        private void OnSelectDriveLetterTarget(object sender, RoutedEventArgs e)
        {
            _vm.UseFolderMountPoint = false;
        }

        private void OnRefreshDrivesClick(object sender, RoutedEventArgs e)
        {
            _vm.RefreshAvailableDriveLetters();
        }

        private void OnBrowseMountFolderClick(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFolderDialog
            {
                Title = LocalizationService.Get("Str_BrowseMountFolderTitle"),
            };
            if (!string.IsNullOrWhiteSpace(_vm.MountFolderPath) && Directory.Exists(_vm.MountFolderPath))
            {
                dlg.InitialDirectory = _vm.MountFolderPath;
            }
            if (dlg.ShowDialog(this) == true)
            {
                _vm.MountFolderPath = dlg.FolderName;
            }
        }

        private void OnBrowseUpperDirClick(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFolderDialog
            {
                Title = LocalizationService.Get("Str_BrowseUpperDirTitle"),
            };
            if (!string.IsNullOrWhiteSpace(_vm.OverlayUpperDir) && Directory.Exists(_vm.OverlayUpperDir))
            {
                dlg.InitialDirectory = _vm.OverlayUpperDir;
            }
            if (dlg.ShowDialog(this) == true)
            {
                _vm.OverlayUpperDir = dlg.FolderName;
            }
        }

        private async void OnBrowseCoreExeClick(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog
            {
                Title = LocalizationService.Get("Str_BrowseCoreTitle"),
                Filter = LocalizationService.Get("Str_BrowseCoreFilter"),
            };
            if (dlg.ShowDialog(this) == true)
            {
                _vm.SetCoreExecutablePath(dlg.FileName);
                await _vm.RefreshAllAsync();
            }
        }

        private void OnCopyCliCommandClick(object sender, RoutedEventArgs e)
        {
            Clipboard.SetText(_vm.CliPreviewCommand);
            _vm.StatusMessage = LocalizationService.Get("Str_StatusCliCopied");
        }

        private void OnBrowseTraceOutputClick(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Title = LocalizationService.Get("Str_BrowseTraceTitle"),
                Filter = LocalizationService.Get("Str_BrowseTraceFilter"),
                DefaultExt = ".json",
                FileName = string.IsNullOrWhiteSpace(_vm.TraceOutputPath)
                    ? "zeroext_trace.json"
                    : Path.GetFileName(_vm.TraceOutputPath),
            };
            if (dlg.ShowDialog(this) == true)
            {
                _vm.TraceOutputPath = dlg.FileName;
            }
        }

        private void OnClearTraceOutputClick(object sender, RoutedEventArgs e)
        {
            _vm.TraceOutputPath = string.Empty;
        }

        private void OnCopyLogsClick(object sender, RoutedEventArgs e)
        {
            if (_vm.LogEntries.Count == 0) return;
            Clipboard.SetText(string.Join(Environment.NewLine, _vm.LogEntries));
            _vm.StatusMessage = LocalizationService.Get("Str_StatusLogsCopied");
        }

        private async void OnWindowClosing(object? sender, CancelEventArgs e)
        {
            if (_allowClose) return;

            if (_vm.ActiveMountCount > 0)
            {
                if (_vm.MinimizeToTrayWhenMounted)
                {
                    e.Cancel = true;
                    Hide();
                    _notifyMinimizedToTray();
                    return;
                }

                MessageBoxResult res = MessageBox.Show(
                    this,
                    LocalizationService.Format("Str_CloseActiveMsgFmt", _vm.ActiveMountCount),
                    LocalizationService.Get("Str_CloseActiveTitle"),
                    MessageBoxButton.YesNoCancel,
                    MessageBoxImage.Question);

                if (res == MessageBoxResult.Cancel)
                {
                    e.Cancel = true;
                    return;
                }
                if (res == MessageBoxResult.No)
                {
                    e.Cancel = true;
                    Hide();
                    _notifyMinimizedToTray();
                    return;
                }

                // Yes: unmount all cleanly before closing
                e.Cancel = true;
                _vm.StatusMessage = LocalizationService.Get("Str_StatusUnmountingAll");
                await _vm.MountManager.UnmountAllAsync();
                _allowClose = true;
                Close();
            }
        }
    }
}
