using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using ZeroExt4.Gui.Models;

namespace ZeroExt4.Gui.Services
{
    public static class ZeroExt4ArgBuilder
    {
        public static List<string> BuildListArgs(string? sourcePath = null, bool json = true)
        {
            var args = new List<string> { "list" };
            if (!string.IsNullOrWhiteSpace(sourcePath))
            {
                args.Add(sourcePath);
            }
            if (json)
            {
                args.Add("--json");
            }
            return args;
        }

        public static List<string> BuildInspectArgs(string sourcePath, uint? partitionNumber = null, bool json = true)
        {
            var args = new List<string> { "inspect", sourcePath };
            if (partitionNumber.HasValue && partitionNumber.Value > 0)
            {
                args.Add("--partition");
                args.Add(partitionNumber.Value.ToString(CultureInfo.InvariantCulture));
            }
            if (json)
            {
                args.Add("--json");
            }
            return args;
        }

        public static List<string> BuildLsArgs(string sourcePath, string ext4Path, uint partitionNumber, bool json = true)
        {
            string targetPath = string.IsNullOrWhiteSpace(ext4Path) ? "/" : ext4Path;
            var args = new List<string> { "ls", sourcePath, targetPath };
            if (partitionNumber > 0)
            {
                args.Add("--partition");
                args.Add(partitionNumber.ToString(CultureInfo.InvariantCulture));
            }
            if (json)
            {
                args.Add("--json");
            }
            return args;
        }

        public static List<string> BuildMkfsArgs(string imagePath, uint sizeMb, string? label)
        {
            var args = new List<string>
            {
                "mkfs",
                imagePath,
                "--size-mb",
                sizeMb.ToString(CultureInfo.InvariantCulture),
            };
            if (!string.IsNullOrWhiteSpace(label))
            {
                args.Add("--label");
                args.Add(label.Trim());
            }
            return args;
        }

        public static List<string> BuildUnmountArgs(string driveLetter)
        {
            return new List<string> { "unmount", NormalizeMountPointArg(driveLetter) };
        }

        public static List<string> BuildMountArgs(
            MountRequest req,
            string? stopEventName = null,
            bool jsonEvents = false,
            uint statsIntervalMs = 0)
        {
            if (req == null) throw new ArgumentNullException(nameof(req));

            var args = new List<string>
            {
                "mount",
                "--source",
                req.SourcePath,
                "--mountpoint",
                NormalizeMountPointArg(req.MountPoint),
            };

            if (req.PartitionNumber > 0)
            {
                args.Add("--partition");
                args.Add(req.PartitionNumber.ToString(CultureInfo.InvariantCulture));
            }

            switch (req.Mode)
            {
                case MountMode.Overlay:
                    if (!string.IsNullOrWhiteSpace(req.OverlayUpperDir))
                    {
                        args.Add("--overlay");
                        args.Add(req.OverlayUpperDir.Trim());
                    }
                    if (!req.EnableDirectIo)
                    {
                        args.Add("--no-direct-io");
                    }
                    break;

                case MountMode.ReadWrite:
                    args.Add("--rw");
                    if (req.RoFallbackOnDirty)
                    {
                        args.Add("--ro-fallback");
                    }
                    if (req.ForceDirty)
                    {
                        args.Add("--force-dirty");
                    }
                    break;

                case MountMode.ReadOnly:
                default:
                    if (!req.EnableDirectIo)
                    {
                        args.Add("--no-direct-io");
                    }
                    break;
            }

            if (req.CaseSensitive)
            {
                args.Add("--case-sensitive");
            }

            if (req.CacheBlocks > 0 && req.CacheBlocks != 2048)
            {
                args.Add("--cache-blocks");
                args.Add(req.CacheBlocks.ToString(CultureInfo.InvariantCulture));
            }

            if (req.EnableDebugLog)
            {
                args.Add("--debug");
            }

            if (!string.IsNullOrWhiteSpace(req.TraceOutputPath))
            {
                args.Add("--trace");
                args.Add(req.TraceOutputPath.Trim());
            }

            if (!string.IsNullOrWhiteSpace(stopEventName))
            {
                args.Add("--stop-event");
                args.Add(stopEventName);
            }

            if (jsonEvents)
            {
                args.Add("--json-events");
            }

            if (statsIntervalMs > 0)
            {
                args.Add("--stats-interval");
                args.Add(statsIntervalMs.ToString(CultureInfo.InvariantCulture));
            }

            return args;
        }

        public static string BuildMountPreviewCommand(MountRequest req, string exeName = "zeroext4.exe")
        {
            List<string> args = BuildMountArgs(req, stopEventName: null, jsonEvents: false, statsIntervalMs: 0);
            var sb = new StringBuilder();
            sb.Append(QuoteIfNeeded(exeName));
            foreach (string arg in args)
            {
                sb.Append(' ');
                sb.Append(QuoteIfNeeded(arg));
            }
            return sb.ToString();
        }

        public static string NormalizeMountPointArg(string mountPoint)
        {
            string trimmed = (mountPoint ?? string.Empty).Trim();
            if (trimmed.Length == 3 && char.IsLetter(trimmed[0]) && trimmed[1] == ':' &&
                (trimmed[2] == '\\' || trimmed[2] == '/'))
            {
                return trimmed.Substring(0, 2).ToUpperInvariant();
            }
            if (trimmed.Length == 2 && char.IsLetter(trimmed[0]) && trimmed[1] == ':')
            {
                return trimmed.ToUpperInvariant();
            }
            if (trimmed.Length == 1 && char.IsLetter(trimmed[0]))
            {
                return char.ToUpperInvariant(trimmed[0]) + ":";
            }
            return trimmed;
        }

        private static string QuoteIfNeeded(string arg)
        {
            if (string.IsNullOrEmpty(arg))
            {
                return "\"\"";
            }
            bool needsQuote = false;
            foreach (char c in arg)
            {
                if (char.IsWhiteSpace(c) || c == '"' || c == '&' || c == '|' || c == '(' || c == ')')
                {
                    needsQuote = true;
                    break;
                }
            }
            if (!needsQuote)
            {
                return arg;
            }
            return "\"" + arg.Replace("\"", "\\\"") + "\"";
        }
    }
}
