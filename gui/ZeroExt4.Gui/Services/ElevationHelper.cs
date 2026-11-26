using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;

namespace ZeroExt4.Gui.Services
{
    public static class ElevationHelper
    {
        public static bool IsCurrentProcessElevated()
        {
            try
            {
                using WindowsIdentity identity = WindowsIdentity.GetCurrent();
                var principal = new WindowsPrincipal(identity);
                return principal.IsInRole(WindowsBuiltInRole.Administrator);
            }
            catch
            {
                return false;
            }
        }

        public static bool RestartElevated(string? arguments = null)
        {
            string? exePath = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(exePath))
            {
                exePath = Process.GetCurrentProcess().MainModule?.FileName;
            }
            if (string.IsNullOrWhiteSpace(exePath))
            {
                return false;
            }

            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = arguments ?? string.Empty,
                UseShellExecute = true,
                Verb = "runas",
                WorkingDirectory = Environment.CurrentDirectory,
            };

            try
            {
                Process? p = Process.Start(psi);
                return p != null;
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == 1223) // ERROR_CANCELLED
            {
                return false;
            }
        }

        public static void OpenInExplorer(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            string target = path.Trim();
            if (target.Length == 2 && char.IsLetter(target[0]) && target[1] == ':')
            {
                target += "\\";
            }
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = $"\"{target}\"",
                UseShellExecute = true,
            });
        }

        public static void OpenUrl(string url)
        {
            if (string.IsNullOrWhiteSpace(url)) return;
            Process.Start(new ProcessStartInfo
            {
                FileName = url,
                UseShellExecute = true,
            });
        }
    }
}
