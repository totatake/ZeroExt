using System;
using System.IO;
using System.Linq;
using System.Windows;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.Views
{
    public partial class ForceUnmountDialog : Window
    {
        private readonly ZeroExtCoreClient _coreClient;

        public ForceUnmountDialog(ZeroExtCoreClient coreClient)
        {
            _coreClient = coreClient ?? throw new ArgumentNullException(nameof(coreClient));
            InitializeComponent();

            for (char c = 'D'; c <= 'Z'; c++)
            {
                DriveCombo.Items.Add($"{c}:");
            }
            string? firstNonC = DriveInfo.GetDrives()
                .Select(d => d.Name.TrimEnd('\\'))
                .FirstOrDefault(n => !string.Equals(n, "C:", StringComparison.OrdinalIgnoreCase));
            DriveCombo.Text = !string.IsNullOrEmpty(firstNonC) ? firstNonC : "M:";
        }

        private async void OnForceUnmountClick(object sender, RoutedEventArgs e)
        {
            string drive = ZeroExtArgBuilder.NormalizeMountPointArg(DriveCombo.Text);
            if (!DriveLetterService.IsDriveLetter(drive) ||
                string.Equals(drive, "C:", StringComparison.OrdinalIgnoreCase))
            {
                ResultText.Text = LocalizationService.Get("Str_ForceUnmountErrInvalid");
                ResultBanner.Visibility = Visibility.Visible;
                return;
            }

            try
            {
                UnmountBtn.IsEnabled = false;
                UnmountBtn.Content = LocalizationService.Get("Str_ForceUnmountBusy");
                CoreCommandResult res = await _coreClient.ForceUnmountAsync(drive);
                if (res.ExitCode == 0)
                {
                    ResultText.Text = string.IsNullOrWhiteSpace(res.StdOut)
                        ? LocalizationService.Format("Str_ForceUnmountSuccessFmt", drive)
                        : res.StdOut.Trim();
                }
                else
                {
                    ResultText.Text = string.IsNullOrWhiteSpace(res.StdErr)
                        ? LocalizationService.Format("Str_ForceUnmountNotFoundFmt", drive, res.ExitCode)
                        : res.StdErr.Trim();
                }
                ResultBanner.Visibility = Visibility.Visible;
            }
            catch (Exception ex)
            {
                ResultText.Text = ex.Message;
                ResultBanner.Visibility = Visibility.Visible;
            }
            finally
            {
                UnmountBtn.IsEnabled = true;
                UnmountBtn.Content = LocalizationService.Get("Str_ForceUnmountDialogBtn");
            }
        }
    }
}
