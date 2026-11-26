using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO.Packaging;
using System.Windows;

namespace ZeroExt4.Gui.Services
{
    public static class LocalizationService
    {
        private static readonly object _sync = new object();
        private static Dictionary<string, string>? _activeStrings;
        private static string _currentLanguage = ResolveEffectiveLanguage("auto");

        public static event EventHandler? LanguageChanged;

        static LocalizationService()
        {
            if (!UriParser.IsKnownScheme("pack"))
            {
                _ = PackUriHelper.UriSchemePack;
            }
            if (Application.ResourceAssembly == null)
            {
                Application.ResourceAssembly = typeof(LocalizationService).Assembly;
            }
        }

        public static string CurrentLanguage
        {
            get
            {
                lock (_sync)
                {
                    return _currentLanguage;
                }
            }
        }

        public static string NormalizeSetting(string? setting)
        {
            if (string.Equals(setting, "en", StringComparison.OrdinalIgnoreCase)) return "en";
            if (string.Equals(setting, "ja", StringComparison.OrdinalIgnoreCase)) return "ja";
            return "auto";
        }

        public static string ResolveEffectiveLanguage(string? setting, CultureInfo? culture = null)
        {
            if (string.Equals(setting, "en", StringComparison.OrdinalIgnoreCase)) return "en";
            if (string.Equals(setting, "ja", StringComparison.OrdinalIgnoreCase)) return "ja";

            CultureInfo ui = culture ?? CultureInfo.CurrentUICulture;
            return string.Equals(ui.TwoLetterISOLanguageName, "ja", StringComparison.OrdinalIgnoreCase)
                ? "ja"
                : "en";
        }

        public static ResourceDictionary LoadLanguageDictionary(string effectiveLang)
        {
            string code = string.Equals(effectiveLang, "ja", StringComparison.OrdinalIgnoreCase) ? "ja" : "en";
            var uri = new Uri($"/ZeroExt4Gui;component/Themes/Strings.{code}.xaml", UriKind.Relative);
            return new ResourceDictionary { Source = uri };
        }

        public static void ApplyLanguage(Application? app, string? languageSetting, CultureInfo? cultureOverride = null)
        {
            string effective = ResolveEffectiveLanguage(languageSetting, cultureOverride);
            ResourceDictionary next = LoadLanguageDictionary(effective);

            if (app != null)
            {
                var dicts = app.Resources.MergedDictionaries;
                ResourceDictionary? existing = null;
                foreach (ResourceDictionary d in dicts)
                {
                    string src = d.Source?.OriginalString ?? string.Empty;
                    if (src.Contains("Strings.en.xaml", StringComparison.OrdinalIgnoreCase) ||
                        src.Contains("Strings.ja.xaml", StringComparison.OrdinalIgnoreCase))
                    {
                        existing = d;
                        break;
                    }
                }

                if (existing != null)
                {
                    int idx = dicts.IndexOf(existing);
                    dicts[idx] = next;
                }
                else
                {
                    dicts.Add(next);
                }
            }

            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (DictionaryEntry entry in next)
            {
                if (entry.Key is string key && entry.Value is string val)
                {
                    map[key] = val;
                }
            }

            lock (_sync)
            {
                _currentLanguage = effective;
                _activeStrings = map;
            }

            LanguageChanged?.Invoke(null, EventArgs.Empty);
        }

        public static string Get(string key)
        {
            Dictionary<string, string>? map;
            lock (_sync)
            {
                map = _activeStrings;
            }

            if (map == null)
            {
                ApplyLanguage(Application.Current, _currentLanguage);
                lock (_sync)
                {
                    map = _activeStrings;
                }
            }

            if (map != null && map.TryGetValue(key, out string? value))
            {
                return value;
            }

            return key;
        }

        public static string Format(string key, params object?[] args)
        {
            string fmt = Get(key);
            try
            {
                return string.Format(CultureInfo.CurrentCulture, fmt, args);
            }
            catch (FormatException)
            {
                return fmt;
            }
        }
    }
}
