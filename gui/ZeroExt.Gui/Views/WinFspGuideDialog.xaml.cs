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
                StatusPillText.Text = LocalizationService.Get("Str_WinFspInstalledBadge");
            }
            else
            {
                StatusPill.SetResourceReference(Border.BackgroundProperty, "WarningBgBrush");
                StatusPillText.SetResourceReference(TextBlock.ForegroundProperty, "WarningBrush");
                StatusPillText.Text = LocalizationService.Get("Str_WinFspNotFoundBadge");
            }
        }

        private void OnCopyCommandClick(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.Tag is string cmd)
            {
                Clipboard.SetText(cmd);
                CopyNoticeText.Text = LocalizationService.Format("Str_WinFspCopiedFmt", cmd);
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
            CopyNoticeText.Text = LocalizationService.Get("Str_WinFspRechecking");
            bool available = await _recheckAsync();
            UpdateStatus(available);
            CopyNoticeText.Text = available
                ? LocalizationService.Get("Str_WinFspDetected")
                : LocalizationService.Get("Str_WinFspStillMissing");
        }
    }
}
