using System;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using ZeroExt.Gui.Models;
using ZeroExt.Gui.Services;
using ZeroExt.Gui.ViewModels;
using ZeroExt.Gui.Views;

namespace ZeroExt.Gui
{
    public partial class App : Application
    {
        private MainViewModel? _viewModel;
        private MainWindow? _mainWindow;
        private TrayIconService? _trayIcon;
        private bool _selfTestMode;

        protected override async void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            string? coreArg = null;
            string? imageArg = null;

            for (int i = 0; i < e.Args.Length; i++)
            {
                string arg = e.Args[i];
                if (string.Equals(arg, "--self-test", StringComparison.OrdinalIgnoreCase))
                {
                    _selfTestMode = true;
                }
                else if (string.Equals(arg, "--core", StringComparison.OrdinalIgnoreCase) && i + 1 < e.Args.Length)
                {
                    coreArg = e.Args[++i];
                }
                else if (string.Equals(arg, "--image", StringComparison.OrdinalIgnoreCase) && i + 1 < e.Args.Length)
                {
                    imageArg = e.Args[++i];
                }
                else if (!arg.StartsWith("-", StringComparison.Ordinal) && File.Exists(arg))
                {
                    imageArg = arg;
                }
            }

            if (_selfTestMode)
            {
                RunSelfTestAndExit(coreArg);
                return;
            }

            DispatcherUnhandledException += OnDispatcherUnhandledException;

            var settingsService = new SettingsService();
            _viewModel = new MainViewModel(
                settingsService,
                isDark => ThemeManager.ApplyTheme(this, isDark),
                coreArg,
                act => Dispatcher.BeginInvoke(act));

            ThemeManager.ApplyTheme(this, _viewModel.IsDarkTheme);

            _mainWindow = new MainWindow(
                _viewModel,
                () => _trayIcon?.ShowMinimizedBalloon(_viewModel.ActiveMountCount));

            _trayIcon = new TrayIconService(
                _viewModel.MountManager,
                ShowMainWindowFromTray,
                mount => ElevationHelper.OpenInExplorer(mount.MountPoint),
                async mount => await _viewModel.UnmountItemAsync(mount),
                async () => await ShutdownAllAndExitAsync());

            MainWindow = _mainWindow;
            _mainWindow.Show();

            await _viewModel.RefreshAllAsync();
            if (!string.IsNullOrWhiteSpace(imageArg))
            {
                await _viewModel.AddImageFileAsync(imageArg, autoSelect: true);
            }
        }

        private void RunSelfTestAndExit(string? coreArg)
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            string tempSettings = Path.Combine(
                Path.GetTempPath(),
                $"zeroext_selftest_{Guid.NewGuid():N}.json");

            try
            {
                var settingsService = new SettingsService(tempSettings);
                using var vm = new MainViewModel(
                    settingsService,
                    isDark => ThemeManager.ApplyTheme(this, isDark),
                    coreArg,
                    act => act());

                ThemeManager.ApplyTheme(this, false);
                var win = new MainWindow(vm);
                win.Measure(new Size(1200, 780));
                win.Arrange(new Rect(0, 0, 1200, 780));
                win.UpdateLayout();

                using var tray = new TrayIconService(
                    vm.MountManager,
                    () => { },
                    _ => { },
                    _ => { },
                    () => { });
                var mockMount = new ActiveMountInfo
                {
                    Id = "selftest-1",
                    SourcePath = @"C:\selftest.img",
                    MountPoint = "M:",
                    VolumeLabel = "SELFTEST",
                    ModeDisplay = "RO",
                    IsReadOnly = true,
                };
                vm.ActiveMounts.Add(mockMount);
                win.UpdateLayout();
                vm.ActiveMounts.Remove(mockMount);
                win.UpdateLayout();

                ThemeManager.ApplyTheme(this, true);
                win.UpdateLayout();
                ThemeManager.ApplyTheme(this, false);

                var createDlg = new CreateImageDialog(vm.CoreClient);
                createDlg.Measure(new Size(520, 500));
                createDlg.UpdateLayout();
                createDlg.Close();

                var unmountDlg = new ForceUnmountDialog(vm.CoreClient);
                unmountDlg.Measure(new Size(480, 400));
                unmountDlg.UpdateLayout();
                unmountDlg.Close();

                var guideDlg = new WinFspGuideDialog(vm.IsWinFspAvailable);
                guideDlg.Measure(new Size(560, 600));
                guideDlg.UpdateLayout();
                guideDlg.Close();

                win.ForceCloseWithoutTray();
                Shutdown(0);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"ZeroExtGui --self-test failed: {ex}");
                Shutdown(1);
            }
            finally
            {
                try { if (File.Exists(tempSettings)) File.Delete(tempSettings); } catch { }
            }
        }

        private void ShowMainWindowFromTray()
        {
            if (_mainWindow == null) return;
            Dispatcher.Invoke(() =>
            {
                if (!_mainWindow.IsVisible)
                {
                    _mainWindow.Show();
                }
                if (_mainWindow.WindowState == WindowState.Minimized)
                {
                    _mainWindow.WindowState = WindowState.Normal;
                }
                _mainWindow.Activate();
            });
        }

        private async Task ShutdownAllAndExitAsync()
        {
            if (_viewModel != null)
            {
                await _viewModel.MountManager.UnmountAllAsync();
            }
            await Dispatcher.InvokeAsync(() =>
            {
                _mainWindow?.ForceCloseWithoutTray();
                Shutdown(0);
            });
        }

        protected override void OnExit(ExitEventArgs e)
        {
            try
            {
                _trayIcon?.Dispose();
                if (_viewModel != null)
                {
                    Task.Run(() => _viewModel.MountManager.UnmountAllAsync()).Wait(TimeSpan.FromSeconds(5));
                    _viewModel.Dispose();
                }
            }
            catch
            {
                // Best effort cleanup
            }
            base.OnExit(e);
        }

        private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
        {
            if (_selfTestMode) return;
            MessageBox.Show(
                $"予期しないエラーが発生しました:\n\n{e.Exception.Message}",
                "ZeroExt GUI エラー",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            e.Handled = true;
        }
    }
}
