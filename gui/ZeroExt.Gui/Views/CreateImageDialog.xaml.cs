using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using ZeroExt.Gui.Services;

namespace ZeroExt.Gui.Views
{
    public partial class CreateImageDialog : Window
    {
        private readonly ZeroExtCoreClient _coreClient;

        public string CreatedImagePath { get; private set; } = string.Empty;

        public CreateImageDialog(ZeroExtCoreClient coreClient)
        {
            _coreClient = coreClient ?? throw new ArgumentNullException(nameof(coreClient));
            InitializeComponent();

            string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            if (string.IsNullOrWhiteSpace(docs)) docs = Environment.CurrentDirectory;
            ImagePathBox.Text = Path.Combine(docs, "zeroext_disk.img");
        }

        private void OnBrowseClick(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Title = "ext4 イメージの保存先を選択",
                Filter = "ディスクイメージ (*.img;*.raw)|*.img;*.raw|すべてのファイル (*.*)|*.*",
                DefaultExt = ".img",
                FileName = Path.GetFileName(ImagePathBox.Text),
            };
            string? dir = Path.GetDirectoryName(ImagePathBox.Text);
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
            {
                dlg.InitialDirectory = dir;
            }
            if (dlg.ShowDialog(this) == true)
            {
                ImagePathBox.Text = dlg.FileName;
            }
        }

        private void OnPresetSizeClick(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.Tag is string sizeStr)
            {
                SizeMbBox.Text = sizeStr;
            }
        }

        private async void OnCreateClick(object sender, RoutedEventArgs e)
        {
            ErrorBanner.Visibility = Visibility.Collapsed;
            string path = ImagePathBox.Text.Trim();
            if (string.IsNullOrWhiteSpace(path))
            {
                ShowError("保存先ファイルパスを入力してください。");
                return;
            }
            if (!uint.TryParse(SizeMbBox.Text.Trim(), out uint sizeMb) || sizeMb < 2)
            {
                ShowError("サイズは 2 MB 以上の整数を指定してください。");
                return;
            }
            string label = LabelBox.Text.Trim();
            if (System.Text.Encoding.UTF8.GetByteCount(label) > 16)
            {
                ShowError("ボリュームラベルは UTF-8 で 16 バイト以内（日本語約 5 文字、英数字 16 文字まで）にしてください。");
                return;
            }

            try
            {
                CreateBtn.IsEnabled = false;
                CancelBtn.IsEnabled = false;
                CreateBtn.Content = "作成中...";

                string? parent = Path.GetDirectoryName(Path.GetFullPath(path));
                if (!string.IsNullOrEmpty(parent))
                {
                    Directory.CreateDirectory(parent);
                }

                await _coreClient.CreateImageAsync(path, sizeMb, label);
                CreatedImagePath = Path.GetFullPath(path);
                DialogResult = true;
                Close();
            }
            catch (Exception ex)
            {
                ShowError(ex.Message);
            }
            finally
            {
                CreateBtn.IsEnabled = true;
                CancelBtn.IsEnabled = true;
                CreateBtn.Content = "イメージを作成";
            }
        }

        private void ShowError(string msg)
        {
            ErrorText.Text = msg;
            ErrorBanner.Visibility = Visibility.Visible;
        }
    }
}
