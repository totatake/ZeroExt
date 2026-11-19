using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using ZeroExt.Gui.Models;

namespace ZeroExt.Gui.Services
{
    public sealed class SettingsService
    {
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            Converters = { new JsonStringEnumConverter() },
        };

        public string SettingsFilePath { get; }

        public SettingsService(string? customSettingsFilePath = null)
        {
            if (!string.IsNullOrWhiteSpace(customSettingsFilePath))
            {
                SettingsFilePath = customSettingsFilePath;
            }
            else
            {
                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (string.IsNullOrWhiteSpace(localAppData))
                {
                    localAppData = AppContext.BaseDirectory;
                }
                SettingsFilePath = Path.Combine(localAppData, "ZeroExt", "gui_settings.json");
            }
        }

        public GuiSettings Load()
        {
            try
            {
                if (!File.Exists(SettingsFilePath))
                {
                    return new GuiSettings();
                }
                string json = File.ReadAllText(SettingsFilePath);
                GuiSettings? settings = JsonSerializer.Deserialize<GuiSettings>(json, JsonOptions);
                return settings ?? new GuiSettings();
            }
            catch
            {
                return new GuiSettings();
            }
        }

        public void Save(GuiSettings settings)
        {
            if (settings == null) throw new ArgumentNullException(nameof(settings));
            string? dir = Path.GetDirectoryName(SettingsFilePath);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            string tmp = SettingsFilePath + ".tmp";
            string json = JsonSerializer.Serialize(settings, JsonOptions);
            File.WriteAllText(tmp, json);
            File.Move(tmp, SettingsFilePath, overwrite: true);
        }
    }
}
