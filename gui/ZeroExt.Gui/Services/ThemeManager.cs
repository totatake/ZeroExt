using System;
using System.Windows;

namespace ZeroExt.Gui.Services
{
    public static class ThemeManager
    {
        private static readonly Uri LightUri = new Uri("Themes/LightTheme.xaml", UriKind.Relative);
        private static readonly Uri DarkUri = new Uri("Themes/DarkTheme.xaml", UriKind.Relative);

        public static void ApplyTheme(Application app, bool isDark)
        {
            if (app == null) return;
            var dicts = app.Resources.MergedDictionaries;
            ResourceDictionary? existingTheme = null;
            foreach (ResourceDictionary d in dicts)
            {
                string src = d.Source?.OriginalString ?? string.Empty;
                if (src.Contains("LightTheme.xaml", StringComparison.OrdinalIgnoreCase) ||
                    src.Contains("DarkTheme.xaml", StringComparison.OrdinalIgnoreCase))
                {
                    existingTheme = d;
                    break;
                }
            }

            var next = new ResourceDictionary
            {
                Source = isDark ? DarkUri : LightUri,
            };

            if (existingTheme != null)
            {
                int idx = dicts.IndexOf(existingTheme);
                dicts[idx] = next;
            }
            else
            {
                dicts.Insert(0, next);
            }
        }
    }
}
