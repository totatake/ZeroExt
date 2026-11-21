using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using ZeroExt.Gui.Models;

namespace ZeroExt.Gui.Services
{
    public sealed class TrayIconService : IDisposable
    {
        private readonly MountSessionManager _mountManager;
        private readonly Action _showWindowAction;
        private readonly Action<ActiveMountInfo> _openMountAction;
        private readonly Action<ActiveMountInfo> _unmountAction;
        private readonly Action _exitApplicationAction;
        private readonly NotifyIcon _notifyIcon;
        private readonly Icon _icon;
        private readonly ContextMenuStrip _menu;
        private readonly Font _boldMenuFont;
        private bool _disposed;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyIcon(IntPtr hIcon);

        public TrayIconService(
            MountSessionManager mountManager,
            Action showWindowAction,
            Action<ActiveMountInfo> openMountAction,
            Action<ActiveMountInfo> unmountAction,
            Action exitApplicationAction)
        {
            _mountManager = mountManager ?? throw new ArgumentNullException(nameof(mountManager));
            _showWindowAction = showWindowAction ?? throw new ArgumentNullException(nameof(showWindowAction));
            _openMountAction = openMountAction ?? throw new ArgumentNullException(nameof(openMountAction));
            _unmountAction = unmountAction ?? throw new ArgumentNullException(nameof(unmountAction));
            _exitApplicationAction = exitApplicationAction ?? throw new ArgumentNullException(nameof(exitApplicationAction));

            _icon = CreateBrandIcon();
            _menu = new ContextMenuStrip();
            _boldMenuFont = new Font(_menu.Font, FontStyle.Bold);
            _notifyIcon = new NotifyIcon
            {
                Icon = _icon,
                Text = LocalizationService.Get("Str_TrayDefaultText"),
                ContextMenuStrip = _menu,
                Visible = true,
            };
            _notifyIcon.DoubleClick += (_, _) => _showWindowAction();

            _mountManager.ActiveMounts.CollectionChanged += OnActiveMountsChanged;
            LocalizationService.LanguageChanged += OnLanguageChanged;
            RebuildMenu();
        }

        public void ShowMinimizedBalloon(int activeCount)
        {
            if (_disposed) return;
            _notifyIcon.ShowBalloonTip(
                2500,
                LocalizationService.Get("Str_TrayBalloonTitle"),
                LocalizationService.Format("Str_TrayBalloonBodyFmt", activeCount),
                ToolTipIcon.Info);
        }

        private void OnLanguageChanged(object? sender, EventArgs e)
        {
            RebuildMenu();
        }

        private void OnActiveMountsChanged(object? sender, NotifyCollectionChangedEventArgs e)
        {
            RebuildMenu();
        }

        private void RebuildMenu()
        {
            if (_disposed) return;

            ToolStripItem[] oldItems = new ToolStripItem[_menu.Items.Count];
            _menu.Items.CopyTo(oldItems, 0);
            _menu.Items.Clear();
            foreach (ToolStripItem old in oldItems)
            {
                old.Dispose();
            }

            var mounts = new List<ActiveMountInfo>(_mountManager.ActiveMounts);
            int count = mounts.Count;
            _notifyIcon.Text = count > 0
                ? LocalizationService.Format("Str_TrayActiveTextFmt", count)
                : LocalizationService.Get("Str_TrayIdleText");

            var header = new ToolStripMenuItem(count > 0
                ? LocalizationService.Format("Str_TrayHeaderActiveFmt", count)
                : LocalizationService.Get("Str_TrayHeaderIdle"))
            {
                Enabled = false,
            };
            _menu.Items.Add(header);
            _menu.Items.Add(new ToolStripSeparator());

            if (count > 0)
            {
                foreach (ActiveMountInfo mount in mounts)
                {
                    var item = new ToolStripMenuItem($"{mount.DisplayTitle} [{mount.ModeDisplay}]");
                    ActiveMountInfo captured = mount;

                    var openItem = new ToolStripMenuItem(LocalizationService.Get("Str_TrayOpenExplorer"));
                    openItem.Click += (_, _) => _openMountAction(captured);

                    var unmountItem = new ToolStripMenuItem(LocalizationService.Get("Str_TraySafeUnmount"));
                    unmountItem.Click += (_, _) => _unmountAction(captured);

                    item.DropDownItems.Add(openItem);
                    item.DropDownItems.Add(unmountItem);
                    _menu.Items.Add(item);
                }
                _menu.Items.Add(new ToolStripSeparator());
            }

            var showItem = new ToolStripMenuItem(LocalizationService.Get("Str_TrayShowWindow"));
            showItem.Font = _boldMenuFont;
            showItem.Click += (_, _) => _showWindowAction();
            _menu.Items.Add(showItem);

            var exitItem = new ToolStripMenuItem(count > 0
                ? LocalizationService.Get("Str_TrayUnmountAllExit")
                : LocalizationService.Get("Str_TrayExit"));
            exitItem.Click += (_, _) => _exitApplicationAction();
            _menu.Items.Add(exitItem);
        }

        private static Icon CreateBrandIcon()
        {
            using var bmp = new Bitmap(32, 32);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);

                using var bgBrush = new SolidBrush(Color.FromArgb(37, 99, 235));
                using var path = RoundedRect(new Rectangle(2, 2, 28, 28), 7);
                g.FillPath(bgBrush, path);

                using var pen = new Pen(Color.FromArgb(147, 197, 253), 1.5f);
                g.DrawPath(pen, path);

                using var font = new Font("Segoe UI", 11f, FontStyle.Bold, GraphicsUnit.Point);
                using var textBrush = new SolidBrush(Color.White);
                using var sf = new StringFormat
                {
                    Alignment = StringAlignment.Center,
                    LineAlignment = StringAlignment.Center,
                };
                g.DrawString("ZE", font, textBrush, new RectangleF(1, 2, 30, 29), sf);
            }

            IntPtr hIcon = bmp.GetHicon();
            try
            {
                using Icon temp = Icon.FromHandle(hIcon);
                return (Icon)temp.Clone();
            }
            finally
            {
                DestroyIcon(hIcon);
            }
        }

        private static GraphicsPath RoundedRect(Rectangle bounds, int radius)
        {
            int d = radius * 2;
            var gp = new GraphicsPath();
            gp.AddArc(bounds.X, bounds.Y, d, d, 180, 90);
            gp.AddArc(bounds.Right - d, bounds.Y, d, d, 270, 90);
            gp.AddArc(bounds.Right - d, bounds.Bottom - d, d, d, 0, 90);
            gp.AddArc(bounds.X, bounds.Bottom - d, d, d, 90, 90);
            gp.CloseFigure();
            return gp;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _mountManager.ActiveMounts.CollectionChanged -= OnActiveMountsChanged;
            LocalizationService.LanguageChanged -= OnLanguageChanged;
            _notifyIcon.Visible = false;
            _notifyIcon.ContextMenuStrip = null;
            _notifyIcon.Dispose();
            _menu.Dispose();
            _boldMenuFont.Dispose();
            _icon.Dispose();
        }
    }
}
