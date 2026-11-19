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
                    : $"{message}\n\n--- 詳細 ---\n{detail}";
                MessageBox.Show(this, full, title, MessageBoxButton.OK, MessageBoxImage.Warning);
            };

            _vm.ConfirmForceKillCallback = mount =>
            {
                MessageBoxResult r = MessageBox.Show(
                    this,
                    $"'{mount.MountPoint}' のアンマウントがタイムアウトしました（エクスプローラーや他アプリがファイルを開いている可能性があります）。\n\n強制終了してドライブ定義をクリーンアップしますか？",
                    "アンマウント待機タイムアウト",
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
                Title = "ext2 / ext3 / ext4 ディスクイメージを開く",
                Filter = "ディスクイメージ (*.img;*.raw;*.ext4;*.bin)|*.img;*.raw;*.ext4;*.bin|すべてのファイル (*.*)|*.*",
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
                MessageBox.Show(this, "zeroext.exe が見つからないためイメージを作成できません。", "エラー",
                    MessageBoxButton.OK, MessageBoxImage.Warning);
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
                MessageBox.Show(this, "zeroext.exe が見つからないため実行できません。", "エラー",
                    MessageBoxButton.OK, MessageBoxImage.Warning);
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
                    $"現在 {_vm.ActiveMountCount} 件のマウントが稼働中です。管理者権限で再起動する前にすべてのマウントを安全に解除しますか？",
                    "管理者権限で再起動",
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
                Title = "マウント先となる空の NTFS フォルダを選択",
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
                Title = "Overlay の書き込み先となる NTFS フォルダを選択",
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
                Title = "zeroext.exe を選択",
                Filter = "ZeroExt 実行ファイル (zeroext.exe)|zeroext.exe|実行ファイル (*.exe)|*.exe",
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
            _vm.StatusMessage = "CLI コマンドをクリップボードにコピーしました";
        }

        private void OnCopyLogsClick(object sender, RoutedEventArgs e)
        {
            if (_vm.LogEntries.Count == 0) return;
            Clipboard.SetText(string.Join(Environment.NewLine, _vm.LogEntries));
            _vm.StatusMessage = "ログをクリップボードにコピーしました";
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
                    $"現在 {_vm.ActiveMountCount} 件のマウントが稼働中です。\n\n「はい」: すべて安全にアンマウントして終了\n「いいえ」: システムトレイに最小化してマウントを継続\n「キャンセル」: ウィンドウに戻る",
                    "マウント継続中の終了確認",
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
                _vm.StatusMessage = "すべてのマウントを安全に解除しています...";
                await _vm.MountManager.UnmountAllAsync();
                _allowClose = true;
                Close();
            }
        }
    }
}
