using System;
using System.Collections.Generic;
using System.IO;

namespace ZeroExt.Gui.Services
{
    public static class CoreExecutableLocator
    {
        public static string? Locate(string? explicitOverride = null, string? savedSettingPath = null)
        {
            foreach (string candidate in EnumerateCandidates(explicitOverride, savedSettingPath))
            {
                if (!string.IsNullOrWhiteSpace(candidate) && File.Exists(candidate))
                {
                    return Path.GetFullPath(candidate);
                }
            }
            return null;
        }

        private static IEnumerable<string> EnumerateCandidates(string? explicitOverride, string? savedSettingPath)
        {
            if (!string.IsNullOrWhiteSpace(explicitOverride))
            {
                yield return explicitOverride;
            }

            string? envPath = Environment.GetEnvironmentVariable("ZEROEXT_CORE_PATH");
            if (!string.IsNullOrWhiteSpace(envPath))
            {
                yield return envPath;
            }

            if (!string.IsNullOrWhiteSpace(savedSettingPath))
            {
                yield return savedSettingPath;
            }

            string baseDir = AppContext.BaseDirectory;
            if (!string.IsNullOrWhiteSpace(baseDir))
            {
                yield return Path.Combine(baseDir, "zeroext.exe");
                string? parent = Directory.GetParent(baseDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))?.FullName;
                if (!string.IsNullOrWhiteSpace(parent))
                {
                    yield return Path.Combine(parent, "zeroext.exe");
                }

                // Walk ancestor directories looking for build\zeroext.exe (repo layout)
                DirectoryInfo? cursor = new DirectoryInfo(baseDir);
                for (int depth = 0; depth < 8 && cursor != null; depth++, cursor = cursor.Parent)
                {
                    yield return Path.Combine(cursor.FullName, "zeroext.exe");
                    yield return Path.Combine(cursor.FullName, "build", "zeroext.exe");
                }
            }

            string? pathEnv = Environment.GetEnvironmentVariable("PATH");
            if (!string.IsNullOrWhiteSpace(pathEnv))
            {
                foreach (string dir in pathEnv.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
                {
                    string trimmed = dir.Trim().Trim('"');
                    if (!string.IsNullOrEmpty(trimmed))
                    {
                        yield return Path.Combine(trimmed, "zeroext.exe");
                    }
                }
            }
        }
    }
}
