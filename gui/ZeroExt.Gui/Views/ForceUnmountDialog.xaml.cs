using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.Views
{
    public partial class ForceUnmountDialog : Window
    {
        private readonly ZeroExtCoreClient _coreClient;
        private bool _updatingCombo;

        public ForceUnmountDialog(ZeroExtCoreClient coreClient)
        {
            _coreClient = coreClient ?? throw new ArgumentNullException(nameof(coreClient));
            InitializeComponent();
            RefreshCandidates(preserveText: false);
        }

        private void RefreshCandidates(bool preserveText)
        {
            string previous = DriveCombo.Text;
            _updatingCombo = true;
            try
            {
                DriveCombo.Items.Clear();
                List<DriveUnmountInspection> candidates = DriveLetterService.FindSafeUnmountCandidates();
                foreach (DriveUnmountInspection c in candidates)
                {
                    DriveCombo.Items.Add(c.DriveLetter);
                }

                if (candidates.Count > 0)
                {
                    DriveCombo.Text = candidates[0].DriveLetter;
                }
                else if (preserveText && !string.IsNullOrWhiteSpace(previous))
                {
                    DriveCombo.Text = previous;
                }
                else
                {
                    DriveCombo.Text = string.Empty;
                }
            }
            finally
            {
                _updatingCombo = false;
            }

            ValidateSelectedDrive(showEmptyAsNoCandidates: true);
        }

        private void OnDriveSelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_updatingCombo) return;
            if (DriveCombo.SelectedItem is string selected)
            {
                _updatingCombo = true;
                try
                {
                    DriveCombo.Text = selected;
                }
                finally
                {
                    _updatingCombo = false;
                }
            }
            ValidateSelectedDrive(showEmptyAsNoCandidates: true);
        }

        private void OnDriveTextChanged(object sender, TextChangedEventArgs e)
        {
            if (_updatingCombo) return;
            ValidateSelectedDrive(showEmptyAsNoCandidates: true);
        }

        private bool ValidateSelectedDrive(bool showEmptyAsNoCandidates)
        {
            string raw = DriveCombo.Text;
            if (string.IsNullOrWhiteSpace(raw))
            {
                UnmountBtn.IsEnabled = false;
                if (showEmptyAsNoCandidates)
                {
                    ResultText.Text = LocalizationService.Get("Str_ForceUnmountNoCandidates");
                    ResultBanner.Visibility = Visibility.Visible;
                }
                else
                {
                    ResultBanner.Visibility = Visibility.Collapsed;
                }
                return false;
            }

            string drive = ZeroExtArgBuilder.NormalizeMountPointArg(raw);
            if (!DriveLetterService.IsDriveLetter(drive) ||
                string.Equals(drive, "C:", StringComparison.OrdinalIgnoreCase))
            {
                UnmountBtn.IsEnabled = false;
                ResultText.Text = LocalizationService.Get("Str_ForceUnmountErrInvalid");
                ResultBanner.Visibility = Visibility.Visible;
                return false;
            }

            DriveUnmountInspection info = DriveLetterService.InspectDriveForUnmount(drive);
            if (!info.HasTarget)
            {
                UnmountBtn.IsEnabled = false;
                ResultText.Text = LocalizationService.Format("Str_ForceUnmountNotFoundFmt", drive, 1);
                ResultBanner.Visibility = Visibility.Visible;
                return false;
            }

            if (!info.IsSafeToUnmount)
            {
                UnmountBtn.IsEnabled = false;
                string fsDisplay = string.IsNullOrWhiteSpace(info.FileSystemName)
                    ? (info.VolumeAlive ? "unknown" : "unmounted")
                    : info.FileSystemName;
                ResultText.Text = LocalizationService.Format("Str_ForceUnmountErrProtectedFmt",
                    drive,
                    string.IsNullOrWhiteSpace(info.DosTarget) ? "-" : info.DosTarget,
                    fsDisplay);
                ResultBanner.Visibility = Visibility.Visible;
                return false;
            }

            UnmountBtn.IsEnabled = true;
            ResultText.Text = info.IsStale
                ? LocalizationService.Format("Str_ForceUnmountTargetStaleFmt", drive, info.DosTarget)
                : LocalizationService.Format("Str_ForceUnmountTargetExt4Fmt", drive, info.DosTarget);
            ResultBanner.Visibility = Visibility.Visible;
            return true;
        }

        private async void OnForceUnmountClick(object sender, RoutedEventArgs e)
        {
            if (!ValidateSelectedDrive(showEmptyAsNoCandidates: true))
            {
                return;
            }

            string drive = ZeroExtArgBuilder.NormalizeMountPointArg(DriveCombo.Text);
            try
            {
                UnmountBtn.IsEnabled = false;
                UnmountBtn.Content = LocalizationService.Get("Str_ForceUnmountBusy");
                CoreCommandResult res = await _coreClient.ForceUnmountAsync(drive);
                if (res.ExitCode == 0)
                {
                    RefreshCandidates(preserveText: false);
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
                UnmountBtn.Content = LocalizationService.Get("Str_ForceUnmountDialogBtn");
                ValidateSelectedDrive(showEmptyAsNoCandidates: false);
            }
        }
    }
}
