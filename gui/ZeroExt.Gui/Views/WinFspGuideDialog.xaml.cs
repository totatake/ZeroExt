using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.Views
{
    public partial class WinFspGuideDialog : Window
    {
        private readonly Func<Task<bool>>? _recheckAsync;

        public WinFspGuideDialog(bool currentlyAvailable, Func<Task<bool>>? recheckAsync = null)
        {
            _recheckAsync = recheckAsync;
            InitializeComponent();
            UpdateStatus(currentlyAvailable);
        }

        private void UpdateStatus(bool available)
        {
            if (available)
            {
                StatusPill.SetResourceReference(Border.BackgroundProperty, "SuccessBgBrush");
                StatusPillText.SetResourceReference(TextBlock.ForegroundProperty, "SuccessBrush");
                StatusPillText.Text = "インストール済み (利用可能)";
            }
            else
            {
                StatusPill.SetResourceReference(Border.BackgroundProperty, "WarningBgBrush");
                StatusPillText.SetResourceReference(TextBlock.ForegroundProperty, "WarningBrush");
                StatusPillText.Text = "未検出 (要インストール)";
            }
        }

        private void OnCopyCommandClick(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.Tag is string cmd)
            {
                Clipboard.SetText(cmd);
                CopyNoticeText.Text = $"コピーしました: {cmd}";
            }
        }

        private void OnOpenOfficialSite(object sender, RoutedEventArgs e)
        {
            ElevationHelper.OpenUrl("https://winfsp.dev/");
        }

        private void OnOpenGithubReleases(object sender, RoutedEventArgs e)
        {
            ElevationHelper.OpenUrl("https://github.com/winfsp/winfsp/releases");
        }

        private async void OnRecheckClick(object sender, RoutedEventArgs e)
        {
            if (_recheckAsync == null) return;
            CopyNoticeText.Text = "再検出中...";
            bool available = await _recheckAsync();
            UpdateStatus(available);
            CopyNoticeText.Text = available ? "WinFsp を検出しました！" : "まだ検出できませんでした。";
        }
    }
}
