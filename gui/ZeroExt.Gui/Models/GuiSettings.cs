using System.Collections.Generic;

namespace ZeroExt.Gui.Models
{
    public sealed class VolumePreset
    {
        public MountMode Mode { get; set; } = MountMode.ReadOnly;
        public string MountPoint { get; set; } = string.Empty;
        public bool UseFolderMountPoint { get; set; }
        public string OverlayUpperDir { get; set; } = string.Empty;
        public bool EnableDirectIo { get; set; } = true;
        public uint CacheBlocks { get; set; } = 2048;
        public bool RoFallbackOnDirty { get; set; } = true;
    }

    public sealed class GuiSettings
    {
        public string Theme { get; set; } = "Light";
        public string Language { get; set; } = "auto";
        public bool MinimizeToTrayWhenMounted { get; set; } = true;
        public bool AutoOpenExplorerOnMount { get; set; } = true;
        public bool ShowNonExtPartitions { get; set; }
        public string? CoreExePath { get; set; }
        public List<string> RegisteredImages { get; set; } = new List<string>();
        public Dictionary<string, VolumePreset> VolumePresets { get; set; } = new Dictionary<string, VolumePreset>();
    }
}
