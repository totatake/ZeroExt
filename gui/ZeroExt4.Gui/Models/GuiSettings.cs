using System.Collections.Generic;

namespace ZeroExt4.Gui.Models
{
    public sealed class VolumePreset
    {
        public MountMode Mode { get; set; } = MountMode.ReadOnly;
        public string MountPoint { get; set; } = string.Empty;
        public bool UseFolderMountPoint { get; set; }
        public string OverlayUpperDir { get; set; } = string.Empty;
        public bool EnableDirectIo { get; set; } = true;
        public bool EnableRwFastIo { get; set; }
        public bool CaseSensitive { get; set; }
        public uint? CreateUid { get; set; }
        public uint? CreateGid { get; set; }
        public bool InheritOwner { get; set; }
        public string CreateFileModeOctal { get; set; } = "0644";
        public string CreateDirModeOctal { get; set; } = "0755";
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
