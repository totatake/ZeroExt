using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using ZeroExt4.Gui.Models;
using ZeroExt4.Gui.Services;
using ZeroExt4.Gui.ViewModels;

namespace ZeroExt4.Gui.Tests
{
    internal static class Program
    {
        private static int _passed;
        private static int _failed;

        private static async Task<int> Main(string[] args)
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
            Console.WriteLine("Running ZeroExt4.Gui automated tests...");

            Run("JsonParsing_ScanResponse", Test_JsonParsing_ScanResponse);
            Run("JsonParsing_LsAndMountEvents", Test_JsonParsing_LsAndMountEvents);
            Run("ArgBuilder_MountVariantsAndPreview", Test_ArgBuilder_MountVariantsAndPreview);
            Run("DriveLetterAndOverlayValidation", Test_DriveLetterAndOverlayValidation);
            Run("DriveLetter_SafeUnmountTarget", Test_DriveLetterService_SafeUnmountTarget);
            Run("Localization_CompletenessAndSwitching", Test_Localization_CompletenessAndSwitching);
            Run("MountErrorTranslator", Test_MountErrorTranslator);
            Run("SettingsService_RoundTrip", Test_SettingsService_RoundTrip);
            Run("MainViewModel_ValidationAndPreview", Test_MainViewModel_ValidationAndPreview);
            Run("TrayIconService_MountUnmountMenuRebuild", Test_TrayIconService_MountUnmountMenuRebuild);
            await RunAsync("LiveCoreCli_InspectAndBrowseIntegration", Test_LiveCoreCli_InspectAndBrowseIntegrationAsync);

            Console.WriteLine();
            Console.WriteLine($"Summary: passed={_passed}, failed={_failed}");
            return _failed == 0 ? 0 : 1;
        }

        private static void Run(string name, Action body)
        {
            try
            {
                body();
                _passed++;
                Console.WriteLine($"[PASS] {name}");
            }
            catch (Exception ex)
            {
                _failed++;
                string detail = ex.InnerException != null ? $"{ex.Message} --> {ex.InnerException.Message}" : ex.Message;
                Console.WriteLine($"[FAIL] {name}: {detail}");
            }
        }

        private static async Task RunAsync(string name, Func<Task> body)
        {
            try
            {
                await body();
                _passed++;
                Console.WriteLine($"[PASS] {name}");
            }
            catch (Exception ex)
            {
                _failed++;
                string detail = ex.InnerException != null ? $"{ex.Message} --> {ex.InnerException.Message}" : ex.Message;
                Console.WriteLine($"[FAIL] {name}: {detail}");
            }
        }

        private static void Assert(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }

        private static void AssertEqual<T>(T expected, T actual, string label)
        {
            if (!EqualityComparer<T>.Default.Equals(expected, actual))
            {
                throw new InvalidOperationException($"{label}: expected '{expected}', got '{actual}'");
            }
        }

        private static void Test_JsonParsing_ScanResponse()
        {
            const string json = """
            {
              "is_elevated": true,
              "winfsp_available": true,
              "disks": [
                {
                  "device_path": "\\\\.\\PhysicalDrive2",
                  "drive_index": 2,
                  "accessible": true,
                  "style": "GPT",
                  "effective_sector_size": 4096,
                  "total_size_bytes": 1073741824,
                  "partitions": [
                    {
                      "partition_number": 1,
                      "offset_bytes": 1048576,
                      "size_bytes": 100000000,
                      "type_id": "EBD0A0A2",
                      "type_description": "Microsoft Basic Data",
                      "partition_name": "EFI",
                      "fs_kind": "unknown",
                      "journal_state": "NONE"
                    },
                    {
                      "partition_number": 2,
                      "offset_bytes": 104857600,
                      "size_bytes": 536870912,
                      "type_id": "0FC63DAF",
                      "type_description": "Linux filesystem",
                      "partition_name": "LinuxRoot",
                      "partition_uuid": "99887766-5544-4333-8222-1100ffeeddcc",
                      "fs_kind": "ext4",
                      "journal_state": "NEEDS_RECOVERY",
                      "volume_label": "テスト領域",
                      "uuid": "11223344-5566-4777-8888-99aabbccddee",
                      "block_size": 4096,
                      "inode_size": 256,
                      "blocks_count": 131072,
                      "total_bytes": 536870912,
                      "free_bytes": 134217728,
                      "feature_incompat_names": ["extents", "64bit"],
                      "last_mount_time": 1700000000,
                      "metadata_csum_enabled": true,
                      "sb_checksum_valid": true,
                      "mount_advice": {
                        "read_only_allowed": true,
                        "overlay_allowed": true,
                        "read_write_allowed": false,
                        "recommended_mode": "read_only",
                        "rw_reason": "Unreplayed journal"
                      }
                    }
                  ]
                }
              ]
            }
            """;

            ScanResponse res = ZeroExt4CoreClient.ParseScanResponse(json);
            Assert(res.IsElevated, "IsElevated should be true");
            Assert(res.WinFspAvailable, "WinFspAvailable should be true");
            AssertEqual(1, res.Disks.Count, "Disks.Count");
            AssertEqual(2, res.Disks[0].Partitions.Count, "Partitions.Count");

            PartitionInfo p0 = res.Disks[0].Partitions[0];
            Assert(!p0.IsExtFs, "p0 should not be IsExtFs");

            PartitionInfo p1 = res.Disks[0].Partitions[1];
            Assert(p1.IsExtFs, "p1 should be IsExtFs");
            Assert(p1.IsDirty, "NEEDS_RECOVERY should mark IsDirty=true");
            AssertEqual("テスト領域", p1.VolumeLabel, "VolumeLabel");
            AssertEqual("11223344-5566-4777-8888-99aabbccddee:131072:4096", p1.ExpectedOverlayLowerId, "ExpectedOverlayLowerId");
            AssertEqual("v2;uuid=11223344-5566-4777-8888-99aabbccddee;blocks=131072;bsize=4096;part_uuid=99887766-5544-4333-8222-1100ffeeddcc;part_off=104857600;part_size=536870912;cs=0", p1.GetExpectedOverlayLowerIdV2(false), "ExpectedOverlayLowerIdV2");
            Assert(p1.FeatureIncompatNames.Contains("extents"), "FeatureIncompatNames should contain extents");
            AssertEqual(1700000000UL, p1.LastMountTime, "LastMountTime");
            Assert(p1.MetadataCsumEnabled && p1.SbChecksumValid, "MetadataCsum flags");
            Assert(p1.MountAdvice != null && !p1.MountAdvice.ReadWriteAllowed && p1.MountAdvice.ReadOnlyAllowed, "MountAdvice parsed");
            AssertEqual(75.0, Math.Round(p1.UsedPercent, 2), "UsedPercent");

            const string docJson = """
            {
              "overall_status": "warn",
              "is_elevated": false,
              "physical_disk_access": false,
              "can_mount_drive": true,
              "winfsp": { "available": true, "dll_path": "C:\\Program Files (x86)\\WinFsp\\bin\\winfsp-x64.dll", "version": "2.0.23075" },
              "overlay": { "upper_dir": "D:\\upper", "case_sensitive": false, "status": "non_empty_unmarked", "can_mount": false, "marker_version": 0, "reason": "unmarked" }
            }
            """;
            DoctorResponse doc = ZeroExt4CoreClient.ParseDoctorResponse(docJson);
            AssertEqual("warn", doc.OverallStatus, "DoctorResponse.OverallStatus");
            Assert(doc.WinFsp.Available && doc.WinFsp.Version == "2.0.23075", "DoctorWinFspInfo parsed");
            Assert(doc.Overlay != null && doc.Overlay.Status == "non_empty_unmarked" && !doc.Overlay.CanMount, "DoctorOverlayInfo parsed");
        }

        private static void Test_JsonParsing_LsAndMountEvents()
        {
            const string lsJson = """
            {
              "source": "D:\\test.img",
              "partition": 0,
              "path": "/資料",
              "entries": [
                { "name": "サブ", "is_dir": true, "is_symlink": false, "size": 0, "mode": 16877, "ino": 12, "hard_links": 2, "last_write_filetime": 133000000000000000 },
                { "name": "readme.txt", "is_dir": false, "is_symlink": false, "size": 2048, "mode": 33188, "ino": 13, "hard_links": 1, "last_write_filetime": 133000000000000000 }
              ]
            }
            """;
            LsResponse ls = ZeroExt4CoreClient.ParseLsResponse(lsJson);
            AssertEqual("/資料", ls.Path, "LsResponse.Path");
            AssertEqual(2, ls.Entries.Count, "LsResponse.Entries.Count");
            Assert(ls.Entries[0].IsDir, "first entry should be dir");
            AssertEqual("2.00 KB", ls.Entries[1].SizeFormatted, "SizeFormatted");

            MountEventMessage? mounted = ZeroExt4CoreClient.TryParseMountEvent(
                "{\"event\":\"mounted\",\"source\":\"test.img\",\"partition\":0,\"mountpoint\":\"M:\",\"mode\":\"RO\",\"read_only\":true,\"direct_io\":true,\"fell_back_to_ro\":false}");
            Assert(mounted != null && mounted.Event == "mounted", "mounted event parsed");
            AssertEqual(true, mounted!.DirectIo ?? false, "DirectIo");

            MountEventMessage? stats = ZeroExt4CoreClient.TryParseMountEvent(
                "{\"event\":\"stats\",\"read_bytes\":1048576,\"write_bytes\":4096,\"read_ops\":10,\"write_ops\":2}");
            Assert(stats != null && stats.ReadBytes == 1048576 && stats.WriteOps == 2, "stats event parsed");

            MountEventMessage? unmounted = ZeroExt4CoreClient.TryParseMountEvent(
                "{\"event\":\"unmounted\",\"mountpoint\":\"M:\",\"trace_path\":\"D:\\\\logs\\\\t.json\",\"trace_events\":1234,\"trace_dropped\":0}");
            Assert(unmounted != null && unmounted.Event == "unmounted", "unmounted event parsed");
            AssertEqual(@"D:\logs\t.json", unmounted!.TracePath, "TracePath");
            AssertEqual(1234UL, unmounted.TraceEvents ?? 0UL, "TraceEvents");
            AssertEqual(0UL, unmounted.TraceDropped ?? 99UL, "TraceDropped");
        }

        private static void Test_ArgBuilder_MountVariantsAndPreview()
        {
            var roReq = new MountRequest
            {
                SourcePath = @"C:\my images\disk.img",
                PartitionNumber = 2,
                MountPoint = "m:",
                Mode = MountMode.ReadOnly,
                EnableDirectIo = true,
                CacheBlocks = 2048,
                TraceOutputPath = @"D:\my traces\zeroext4 trace.json",
            };
            List<string> roArgs = ZeroExt4ArgBuilder.BuildMountArgs(roReq, "Local\\Evt1", true, 1000);
            Assert(!roArgs.Contains("--rw"), "RO should not have --rw");
            Assert(!roArgs.Contains("--no-direct-io"), "RO with DirectIO on should not pass --no-direct-io");
            Assert(roArgs.Contains("--stop-event") && roArgs.Contains("Local\\Evt1"), "stop-event present");
            Assert(roArgs.Contains("--json-events"), "json-events present");
            Assert(roArgs.Contains("--stats-interval") && roArgs.Contains("1000"), "stats-interval present");
            Assert(roArgs.Contains("--trace") && roArgs.Contains(@"D:\my traces\zeroext4 trace.json"), "trace arg present");
            AssertEqual("M:", roArgs[roArgs.IndexOf("--mountpoint") + 1], "normalized drive letter");

            string preview = ZeroExt4ArgBuilder.BuildMountPreviewCommand(roReq);
            Assert(preview.Contains("\"C:\\my images\\disk.img\""), "preview should quote space paths");
            Assert(preview.Contains("--trace \"D:\\my traces\\zeroext4 trace.json\""), "preview should quote trace path");
            Assert(!preview.Contains("--stop-event"), "preview should omit internal --stop-event");
            Assert(!preview.Contains("--json-events"), "preview should omit internal --json-events");

            roReq.EnableDirectIo = false;
            roReq.CaseSensitive = true;
            List<string> roNoDirect = ZeroExt4ArgBuilder.BuildMountArgs(roReq);
            Assert(roNoDirect.Contains("--no-direct-io"), "RO with DirectIO off must pass --no-direct-io");
            Assert(roNoDirect.Contains("--case-sensitive"), "CaseSensitive must pass --case-sensitive");

            var ovReq = new MountRequest
            {
                SourcePath = @"\\.\PhysicalDrive2",
                PartitionNumber = 2,
                MountPoint = "N:",
                Mode = MountMode.Overlay,
                OverlayUpperDir = @"D:\ext4_upper",
                EnableDirectIo = true,
                CacheBlocks = 4096,
            };
            List<string> ovArgs = ZeroExt4ArgBuilder.BuildMountArgs(ovReq);
            Assert(ovArgs.Contains("--overlay") && ovArgs.Contains(@"D:\ext4_upper"), "Overlay args present");
            Assert(!ovArgs.Contains("--rw"), "Overlay must never combine --rw");
            Assert(ovArgs.Contains("--cache-blocks") && ovArgs.Contains("4096"), "custom cache blocks");

            var rwReq = new MountRequest
            {
                SourcePath = "test.img",
                MountPoint = "P:",
                Mode = MountMode.ReadWrite,
                EnableDirectIo = true,
                RoFallbackOnDirty = true,
                ForceDirty = false,
            };
            List<string> rwArgs = ZeroExt4ArgBuilder.BuildMountArgs(rwReq);
            Assert(rwArgs.Contains("--rw"), "RW must include --rw");
            Assert(rwArgs.Contains("--ro-fallback"), "RW with fallback must include --ro-fallback");
            Assert(!rwArgs.Contains("--direct-io") && !rwArgs.Contains("--no-direct-io"), "RW must not pass direct-io flags");
            Assert(!rwArgs.Contains("--rw-fast-io"), "Default RW must not pass --rw-fast-io");

            rwReq.EnableRwFastIo = true;
            List<string> rwFastArgs = ZeroExt4ArgBuilder.BuildMountArgs(rwReq);
            Assert(rwFastArgs.Contains("--rw-fast-io"), "RW with EnableRwFastIo must pass --rw-fast-io");

            roReq.EnableRwFastIo = true;
            ovReq.EnableRwFastIo = true;
            Assert(!ZeroExt4ArgBuilder.BuildMountArgs(roReq).Contains("--rw-fast-io"), "RO must not pass --rw-fast-io");
            Assert(!ZeroExt4ArgBuilder.BuildMountArgs(ovReq).Contains("--rw-fast-io"), "Overlay must not pass --rw-fast-io");

            List<string> docArgs = ZeroExt4ArgBuilder.BuildDoctorArgs(@"D:\test.img", 2, @"D:\ov", caseSensitive: true, json: true);
            AssertEqual("doctor", docArgs[0], "doctor subcommand");
            Assert(docArgs.Contains("--source") && docArgs.Contains(@"D:\test.img"), "doctor source");
            Assert(docArgs.Contains("--partition") && docArgs.Contains("2"), "doctor partition");
            Assert(docArgs.Contains("--overlay") && docArgs.Contains(@"D:\ov"), "doctor overlay");
            Assert(docArgs.Contains("--case-sensitive"), "doctor cs");
            Assert(docArgs.Contains("--json"), "doctor json");

            List<string> ovDiffArgs = ZeroExt4ArgBuilder.BuildOverlayStatusArgs(@"D:\ov", @"D:\test.img", 2, true, diff: true, json: true);
            AssertEqual("overlay", ovDiffArgs[0], "overlay cmd");
            AssertEqual("diff", ovDiffArgs[1], "overlay diff sub");
            Assert(ovDiffArgs.Contains("--upper") && ovDiffArgs.Contains(@"D:\ov"), "overlay upper");
            Assert(ovDiffArgs.Contains("--lower") && ovDiffArgs.Contains(@"D:\test.img"), "overlay lower");

            List<string> ovExportArgs = ZeroExt4ArgBuilder.BuildOverlayExportArgs(@"D:\ov", @"D:\out.tar", @"D:\test.img", 2, false, true);
            AssertEqual("export", ovExportArgs[1], "overlay export sub");
            Assert(ovExportArgs.Contains("--output") && ovExportArgs.Contains(@"D:\out.tar"), "overlay output tar");

            List<string> ovResetArgs = ZeroExt4ArgBuilder.BuildOverlayResetArgs(@"D:\ov", yes: true, json: true);
            AssertEqual("reset", ovResetArgs[1], "overlay reset sub");
            Assert(ovResetArgs.Contains("--yes"), "overlay reset yes");
        }

        private static void Test_DriveLetterAndOverlayValidation()
        {
            List<string> free = DriveLetterService.GetAvailableDriveLetters(new[] { "M:", "N:" });
            Assert(!free.Contains("C:"), "C: must not be in available drive letters");
            Assert(!free.Contains("M:"), "excluded M: must not be offered");
            Assert(!free.Contains("N:"), "excluded N: must not be offered");
            Assert(free.Count > 0, "at least one drive letter should be free");

            Assert(DriveLetterService.IsDriveLetter("M:"), "M: valid");
            Assert(DriveLetterService.IsDriveLetter("m:\\"), "m:\\ valid");
            Assert(!DriveLetterService.IsDriveLetter("C:\\mount\\dir"), "folder not drive letter");

            string rootDir = Path.Combine(Path.GetTempPath(), $"zeroext4_gui_test_{Guid.NewGuid():N}");
            Directory.CreateDirectory(rootDir);
            try
            {
                string emptyFolder = Path.Combine(rootDir, "empty_mp");
                Directory.CreateDirectory(emptyFolder);
                var (mpOk, _) = DriveLetterService.ValidateMountFolder(emptyFolder);
                Assert(mpOk, "empty folder should be valid mountpoint");

                string nonEmptyMp = Path.Combine(rootDir, "non_empty_mp");
                Directory.CreateDirectory(nonEmptyMp);
                File.WriteAllText(Path.Combine(nonEmptyMp, "a.txt"), "x");
                var (mpBad, _) = DriveLetterService.ValidateMountFolder(nonEmptyMp);
                Assert(!mpBad, "non-empty folder should not be valid mountpoint");

                string newUpper = Path.Combine(rootDir, "upper_new");
                UpperDirCheckResult r1 = DriveLetterService.CheckOverlayUpperDir(newUpper, "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.WillCreate, r1.Status, "new upper");
                Assert(r1.CanMount, "WillCreate should allow mount");

                string emptyUpper = Path.Combine(rootDir, "upper_empty");
                Directory.CreateDirectory(emptyUpper);
                UpperDirCheckResult r2 = DriveLetterService.CheckOverlayUpperDir(emptyUpper, "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.EmptyReady, r2.Status, "empty upper");
                Assert(r2.CanMount, "EmptyReady should allow mount");

                string matchUpper = Path.Combine(rootDir, "upper_match");
                Directory.CreateDirectory(matchUpper);
                File.WriteAllText(Path.Combine(matchUpper, ".wh.__ext4_lower_id"), "uuid1:100:4096");
                UpperDirCheckResult r3 = DriveLetterService.CheckOverlayUpperDir(matchUpper, "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.MatchingExisting, r3.Status, "matching upper");
                Assert(r3.CanMount, "MatchingExisting should allow mount");

                UpperDirCheckResult r4 = DriveLetterService.CheckOverlayUpperDir(matchUpper, "uuid2:200:4096");
                AssertEqual(UpperDirCheckStatus.MismatchedLower, r4.Status, "mismatched upper");
                Assert(!r4.CanMount, "MismatchedLower must block mount");

                UpperDirCheckResult r4cs = DriveLetterService.CheckOverlayUpperDir(matchUpper, "uuid1:100:4096", caseSensitive: true);
                AssertEqual(UpperDirCheckStatus.MismatchedLower, r4cs.Status, "non-cs upper must mismatch cs mode");
                Assert(!r4cs.CanMount, "non-cs upper must block cs mount");

                string csMatchUpper = Path.Combine(rootDir, "upper_match_cs");
                Directory.CreateDirectory(csMatchUpper);
                File.WriteAllText(Path.Combine(csMatchUpper, ".wh.__ext4_lower_id"), "uuid1:100:4096:cs");
                UpperDirCheckResult r4csOk = DriveLetterService.CheckOverlayUpperDir(csMatchUpper, "uuid1:100:4096", caseSensitive: true);
                AssertEqual(UpperDirCheckStatus.MatchingExisting, r4csOk.Status, "cs upper matches cs mode");
                Assert(r4csOk.CanMount, "cs upper allows cs mount");

                UpperDirCheckResult r4csToCi = DriveLetterService.CheckOverlayUpperDir(csMatchUpper, "uuid1:100:4096", caseSensitive: false);
                AssertEqual(UpperDirCheckStatus.MismatchedLower, r4csToCi.Status, "cs upper must mismatch non-cs mode");
                Assert(!r4csToCi.CanMount, "cs upper must block non-cs mount");

                string unmarkedUpper = Path.Combine(rootDir, "upper_unmarked");
                Directory.CreateDirectory(unmarkedUpper);
                File.WriteAllText(Path.Combine(unmarkedUpper, "existing.txt"), "hi");
                UpperDirCheckResult r5 = DriveLetterService.CheckOverlayUpperDir(unmarkedUpper, "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.NonEmptyUnmarked, r5.Status, "non-empty unmarked");
                Assert(!r5.CanMount, "NonEmptyUnmarked must block mount unconditionally");

                UpperDirCheckResult r5cs = DriveLetterService.CheckOverlayUpperDir(unmarkedUpper, "uuid1:100:4096", caseSensitive: true);
                Assert(!r5cs.CanMount, "non-empty non-CS folder must be blocked in case-sensitive mode");

                // v2 marker check including GPT partition UUID discrimination
                var partA = new PartitionInfo
                {
                    PartitionNumber = 2,
                    OffsetBytes = 2097152,
                    SizeBytes = 67108864,
                    PartitionUuid = "aaaaaaaa-0000-4000-8000-000000000001",
                    FsKind = "ext4",
                    Uuid = "11111111-2222-4333-8444-555555555555",
                    BlockSize = 4096,
                    BlocksCount = 16384,
                };
                var partBClone = new PartitionInfo
                {
                    PartitionNumber = 2,
                    OffsetBytes = 2097152,
                    SizeBytes = 67108864,
                    PartitionUuid = "bbbbbbbb-0000-4000-8000-000000000002",
                    FsKind = "ext4",
                    Uuid = partA.Uuid,
                    BlockSize = partA.BlockSize,
                    BlocksCount = partA.BlocksCount,
                };
                string v2Dir = Path.Combine(rootDir, "upper_v2");
                Directory.CreateDirectory(v2Dir);
                File.WriteAllText(Path.Combine(v2Dir, ".wh.__ext4_lower_id"), partA.GetExpectedOverlayLowerIdV2(false));
                UpperDirCheckResult rv2Ok = DriveLetterService.CheckOverlayUpperDir(v2Dir, partA, false);
                AssertEqual(UpperDirCheckStatus.MatchingExisting, rv2Ok.Status, "v2 matches partA");
                Assert(rv2Ok.CanMount, "v2 matching partA allows mount");
                UpperDirCheckResult rv2Clone = DriveLetterService.CheckOverlayUpperDir(v2Dir, partBClone, false);
                AssertEqual(UpperDirCheckStatus.MismatchedLower, rv2Clone.Status, "v2 rejects same-UUID different part_uuid");
                Assert(!rv2Clone.CanMount, "v2 clone mismatch blocks mount");

                // Corrupted & unsupported version marker checks
                File.WriteAllText(Path.Combine(v2Dir, ".wh.__ext4_lower_id"), "corrupt-marker-text");
                UpperDirCheckResult rCorrupt = DriveLetterService.CheckOverlayUpperDir(v2Dir, partA, false);
                AssertEqual(UpperDirCheckStatus.CorruptMarker, rCorrupt.Status, "corrupt marker detected");
                Assert(!rCorrupt.CanMount, "corrupt marker blocks mount");

                File.WriteAllText(Path.Combine(v2Dir, ".wh.__ext4_lower_id"), "");
                UpperDirCheckResult rEmptyMarker = DriveLetterService.CheckOverlayUpperDir(v2Dir, partA, false);
                AssertEqual(UpperDirCheckStatus.CorruptMarker, rEmptyMarker.Status, "empty marker rejected as corrupt");
                Assert(!rEmptyMarker.CanMount, "empty marker blocks mount");

                File.WriteAllText(Path.Combine(v2Dir, ".wh.__ext4_lower_id"), "v99;uuid=11111111-2222-4333-8444-555555555555");
                UpperDirCheckResult rFuture = DriveLetterService.CheckOverlayUpperDir(v2Dir, partA, false);
                AssertEqual(UpperDirCheckStatus.UnsupportedMarkerVersion, rFuture.Status, "v99 unsupported version detected");
                Assert(!rFuture.CanMount, "v99 blocks mount");

                UpperDirCheckResult r6 = DriveLetterService.CheckOverlayUpperDir("C:\\", "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.InvalidPath, r6.Status, "drive root rejected");
                Assert(!r6.CanMount, "drive root must block mount");
            }
            finally
            {
                try { Directory.Delete(rootDir, recursive: true); } catch { }
            }
        }

        private static void Test_Localization_CompletenessAndSwitching()
        {
            AssertEqual("ja", LocalizationService.ResolveEffectiveLanguage("auto", CultureInfo.GetCultureInfo("ja-JP")), "auto ja-JP");
            AssertEqual("en", LocalizationService.ResolveEffectiveLanguage("auto", CultureInfo.GetCultureInfo("en-US")), "auto en-US");
            AssertEqual("en", LocalizationService.ResolveEffectiveLanguage("auto", CultureInfo.GetCultureInfo("fr-FR")), "auto fr-FR fallback");
            AssertEqual("ja", LocalizationService.ResolveEffectiveLanguage("ja", CultureInfo.GetCultureInfo("en-US")), "explicit ja");
            AssertEqual("en", LocalizationService.ResolveEffectiveLanguage("en", CultureInfo.GetCultureInfo("ja-JP")), "explicit en");

            ResourceDictionary enDict = LocalizationService.LoadLanguageDictionary("en");
            ResourceDictionary jaDict = LocalizationService.LoadLanguageDictionary("ja");

            var enKeys = new HashSet<string>(StringComparer.Ordinal);
            foreach (DictionaryEntry e in enDict)
            {
                if (e.Key is string k)
                {
                    enKeys.Add(k);
                    Assert(e.Value is string s && !string.IsNullOrWhiteSpace(s), $"en key '{k}' must be non-empty string");
                }
            }

            var jaKeys = new HashSet<string>(StringComparer.Ordinal);
            foreach (DictionaryEntry e in jaDict)
            {
                if (e.Key is string k)
                {
                    jaKeys.Add(k);
                    Assert(e.Value is string s && !string.IsNullOrWhiteSpace(s), $"ja key '{k}' must be non-empty string");
                }
            }

            var missingInJa = enKeys.Except(jaKeys).ToList();
            var missingInEn = jaKeys.Except(enKeys).ToList();
            Assert(missingInJa.Count == 0, $"Keys missing in Strings.ja.xaml: {string.Join(", ", missingInJa)}");
            Assert(missingInEn.Count == 0, $"Keys missing in Strings.en.xaml: {string.Join(", ", missingInEn)}");
            Assert(enKeys.Count >= 100, $"Expected >= 100 localization keys, got {enKeys.Count}");

            LocalizationService.ApplyLanguage(null, "en");
            AssertEqual("en", LocalizationService.CurrentLanguage, "CurrentLanguage en");
            AssertEqual("Ready", LocalizationService.Get("Str_StatusReady"), "en Ready");

            LocalizationService.ApplyLanguage(null, "ja");
            AssertEqual("ja", LocalizationService.CurrentLanguage, "CurrentLanguage ja");
            AssertEqual("準備完了", LocalizationService.Get("Str_StatusReady"), "ja Ready");
        }

        private static void Test_MountErrorTranslator()
        {
            LocalizationService.ApplyLanguage(null, "ja");
            var e1 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -22, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Overlay upperdir 'D:\\u' belongs to a different lower ext4 filesystem" });
            Assert(e1.Title.Contains("一致しません"), "overlay mismatch error translated (ja)");

            var e2 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -30, Message = "EROFS" },
                Array.Empty<string>());
            Assert(e2.Title.Contains("Dirty"), "dirty journal error translated (ja)");

            var e3 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -19, Message = "WinFsp mount failed" },
                Array.Empty<string>());
            Assert(e3.Title.Contains("WinFsp"), "winfsp missing error translated (ja)");

            var e4 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -5, Message = "FspFileSystemSetMountPoint failed: NTSTATUS=0xC0000022" },
                Array.Empty<string>());
            Assert(e4.Title.Contains("マウント先"), "0xC0000022 error translated (ja)");

            LocalizationService.ApplyLanguage(null, "en");
            var e1En = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -22, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Overlay upperdir 'D:\\u' belongs to a different lower ext4 filesystem" });
            Assert(e1En.Title.Contains("Mismatch"), "overlay mismatch error translated (en)");

            var e4En = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -5, Message = "FspFileSystemSetMountPoint failed: NTSTATUS=0xC0000022" },
                Array.Empty<string>());
            Assert(e4En.Title.Contains("Unavailable"), "0xC0000022 error translated (en)");

            LocalizationService.ApplyLanguage(null, "ja");
            var eCsNotEmptyJa = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -41, Message = "Failed to mount" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=145)." });
            Assert(eCsNotEmptyJa.Title.Contains("空ではありません"), "cs winerr=145 translated (ja)");
            Assert(eCsNotEmptyJa.Suggestion.Contains("削除"), "cs winerr=145 suggestion mentions delete (ja)");

            var eCsAccessJa = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -13, Message = "Failed to mount" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=5)." });
            Assert(eCsAccessJa.Title.Contains("権限"), "cs winerr=5 translated (ja)");

            var eCsUnsupJa = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -95, Message = "Failed to mount" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=50)." });
            Assert(eCsUnsupJa.Title.Contains("対応していません"), "cs winerr=50 translated (ja)");

            LocalizationService.ApplyLanguage(null, "en");
            var eCsNotEmptyEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -41, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=145)." });
            Assert(eCsNotEmptyEn.Title.Contains("Not Empty"), "cs winerr=145 translated (en)");

            var eCsAccessEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -13, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=5)." });
            Assert(eCsAccessEn.Title.Contains("Permissions"), "cs winerr=5 translated (en)");

            var eCsUnsupEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -95, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Failed to enable NTFS case sensitivity on 'D:\\u' (winerr=50)." });
            Assert(eCsUnsupEn.Title.Contains("Not Supported"), "cs winerr=50 translated (en)");

            var eOvVerEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -95, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Unsupported .wh.__ext4_lower_id marker format version" });
            Assert(eOvVerEn.Title.Contains("Unsupported"), "overlay unsupported version translated (en)");

            var eOvCorruptEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -22, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Corrupted .wh.__ext4_lower_id v2 marker" });
            Assert(eOvCorruptEn.Title.Contains("Corrupted"), "overlay corrupt marker translated (en)");

            var eOvLegacyEn = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -41, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Refusing non-empty upperdir without .wh.__ext4_lower_id marker; choose an empty or new directory." });
            Assert(eOvLegacyEn.Title.Contains("Unmarked"), "overlay legacy refused translated (en)");

            LocalizationService.ApplyLanguage(null, "ja");
            var eOvLegacyJa = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -41, Message = "Failed" },
                new[] { "[zeroext4:ERROR] Refusing non-empty upperdir without .wh.__ext4_lower_id marker; choose an empty or new directory." });
            Assert(eOvLegacyJa.Title.Contains("非空"), "overlay legacy refused translated (ja)");
        }

        private static void Test_SettingsService_RoundTrip()
        {
            string tmp = Path.Combine(Path.GetTempPath(), $"zeroext4_settings_{Guid.NewGuid():N}.json");
            try
            {
                var svc = new SettingsService(tmp);
                var s = new GuiSettings
                {
                    Theme = "Dark",
                    Language = "en",
                    MinimizeToTrayWhenMounted = false,
                    AutoOpenExplorerOnMount = false,
                    ShowNonExtPartitions = true,
                    RegisteredImages = new List<string> { @"D:\images\ubuntu.img" },
                };
                s.VolumePresets[@"D:\images\ubuntu.img#0"] = new VolumePreset
                {
                    Mode = MountMode.Overlay,
                    MountPoint = "N:",
                    OverlayUpperDir = @"D:\upper",
                    EnableDirectIo = true,
                    CacheBlocks = 4096,
                    RoFallbackOnDirty = false,
                };
                svc.Save(s);

                GuiSettings loaded = svc.Load();
                AssertEqual("Dark", loaded.Theme, "Theme");
                AssertEqual("en", loaded.Language, "Language");
                AssertEqual(false, loaded.MinimizeToTrayWhenMounted, "MinimizeToTrayWhenMounted");
                AssertEqual(true, loaded.ShowNonExtPartitions, "ShowNonExtPartitions");
                AssertEqual(1, loaded.RegisteredImages.Count, "RegisteredImages.Count");
                Assert(loaded.VolumePresets.TryGetValue(@"D:\images\ubuntu.img#0", out VolumePreset? p), "preset found");
                AssertEqual(MountMode.Overlay, p!.Mode, "Preset.Mode");
                AssertEqual("N:", p.MountPoint, "Preset.MountPoint");
                AssertEqual(4096U, p.CacheBlocks, "Preset.CacheBlocks");
            }
            finally
            {
                try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
            }
        }

        private static void Test_MainViewModel_ValidationAndPreview()
        {
            string tmpSettings = Path.Combine(Path.GetTempPath(), $"zeroext4_vm_{Guid.NewGuid():N}.json");
            string? exe = CoreExecutableLocator.Locate();
            try
            {
                var svc = new SettingsService(tmpSettings);
                using var vm = new MainViewModel(svc, null, exe, act => act());

                var disk = new DiskInfo
                {
                    DevicePath = @"C:\dummy.img",
                    DriveIndex = -1,
                    Accessible = true,
                    Style = "RAW",
                    TotalSizeBytes = 64 * 1024 * 1024,
                    Partitions = new List<PartitionInfo>
                    {
                        new PartitionInfo { PartitionNumber = 1, FsKind = "unknown", SizeBytes = 1024 },
                        new PartitionInfo
                        {
                            PartitionNumber = 2,
                            FsKind = "ext4",
                            JournalState = "NEEDS_RECOVERY",
                            VolumeLabel = "DATA",
                            Uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
                            BlockSize = 4096,
                            BlocksCount = 16384,
                            TotalBytes = 67108864,
                            FreeBytes = 33554432,
                        },
                    },
                };

                var group = new DiskGroupItem(disk);
                group.ApplyFilter(false);
                AssertEqual(1, group.VisiblePartitions.Count, "filter hides non-ext");
                AssertEqual(1, group.HiddenNonExtCount, "hidden count = 1");

                group.ApplyFilter(true);
                AssertEqual(2, group.VisiblePartitions.Count, "filter off shows all");

                vm.DiskGroups.Add(group);
                vm.SelectedPartition = group.AllPartitions[1];

                Assert(vm.SelectedIsDirty, "SelectedIsDirty true");
                Assert(vm.IsReadWriteBlockedByHealth, "IsReadWriteBlockedByHealth true on dirty partition");
                Assert(!vm.CanSelectReadWrite, "CanSelectReadWrite false on dirty partition");
                vm.SelectedMode = MountMode.ReadWrite;
                Assert(vm.ShowDirtyRwWarning, "ShowDirtyRwWarning visible in RW mode");
                Assert(!vm.DirectIoEditable, "DirectIO disabled in RW mode");
                Assert(!vm.EffectiveDirectIo, "EffectiveDirectIo false in RW mode");
                Assert(vm.CliPreviewCommand.Contains("--rw"), "CLI preview shows --rw");
                Assert(!vm.CanMount, "Dirty partition RW mount must be blocked unless ForceDirty is enabled");
                vm.ForceDirty = true;
                Assert(vm.CanMount, "Dirty partition RW mount allowed only when ForceDirty is explicitly checked");
                vm.ForceDirty = false;

                // Preset with RW saved for a dirty partition must automatically coerce to ReadOnly
                vm.SelectedPartition = null;
                var svcWithPreset = new SettingsService(tmpSettings);
                svcWithPreset.Save(new GuiSettings
                {
                    VolumePresets = new Dictionary<string, VolumePreset>
                    {
                        [group.AllPartitions[1].UniqueKey] = new VolumePreset { Mode = MountMode.ReadWrite },
                    },
                });
                using var vmCoerce = new MainViewModel(svcWithPreset, null, exe, act => act());
                vmCoerce.DiskGroups.Add(group);
                vmCoerce.SelectedPartition = group.AllPartitions[1];
                AssertEqual(MountMode.ReadOnly, vmCoerce.SelectedMode, "Dirty partition coerces saved RW preset to RO");

                vm.SelectedMode = MountMode.Overlay;
                Assert(vm.DirectIoEditable, "DirectIO editable in Overlay mode");
                Assert(vm.EffectiveDirectIo, "EffectiveDirectIo on by default in Overlay");
                vm.OverlayUpperDir = string.Empty;
                Assert(!vm.CanMount, "Overlay requires non-empty upperdir");
            }
            finally
            {
                try { if (File.Exists(tmpSettings)) File.Delete(tmpSettings); } catch { }
            }
        }

        private static void Test_TrayIconService_MountUnmountMenuRebuild()
        {
            Exception? captured = null;
            var thread = new Thread(() =>
            {
                try
                {
                    using var mgr = new MountSessionManager(() => null, null, act => act());
                    using var tray = new TrayIconService(mgr, () => { }, _ => { }, _ => { }, () => { });

                    var m1 = new ActiveMountInfo
                    {
                        Id = "m1",
                        SourcePath = @"C:\a.img",
                        MountPoint = "M:",
                        ModeDisplay = "RO",
                    };
                    var m2 = new ActiveMountInfo
                    {
                        Id = "m2",
                        SourcePath = @"C:\a.img",
                        MountPoint = "N:",
                        ModeDisplay = "RO",
                    };
                    var m3 = new ActiveMountInfo
                    {
                        Id = "m3",
                        SourcePath = @"C:\a.img",
                        MountPoint = "P:",
                        ModeDisplay = "RO",
                    };

                    mgr.ActiveMounts.Add(m1);
                    mgr.ActiveMounts.Add(m2);
                    mgr.ActiveMounts.Remove(m1);
                    mgr.ActiveMounts.Remove(m2);
                    mgr.ActiveMounts.Add(m3);
                    mgr.ActiveMounts.Clear();

                    AssertEqual(0, mgr.ActiveMounts.Count, "ActiveMounts empty after clear");
                }
                catch (Exception ex)
                {
                    captured = ex;
                }
            });
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start();
            thread.Join();

            if (captured != null)
            {
                ExceptionDispatchInfo.Capture(captured).Throw();
            }
        }

        private static async Task Test_LiveCoreCli_InspectAndBrowseIntegrationAsync()
        {
            string? exe = CoreExecutableLocator.Locate();
            Assert(!string.IsNullOrWhiteSpace(exe), "zeroext4.exe must be found for integration test");

            string tmpDir = Path.Combine(Path.GetTempPath(), $"zeroext4_gui_int_{Guid.NewGuid():N}");
            Directory.CreateDirectory(tmpDir);
            string imgPath = Path.Combine(tmpDir, "統合テスト.img");
            string imgLongLabelPath = Path.Combine(tmpDir, "long_label.img");

            try
            {
                var client = new ZeroExt4CoreClient(() => exe);
                await client.CreateImageAsync(imgPath, 16, "テストEXT4");

                CoreCommandResult r1 = await client.RunCommandAsync(new[] { "mkdir", imgPath, "/ドキュメント" });
                AssertEqual(0, r1.ExitCode, "mkdir exit");

                const string helloText = "Hello ZeroExt4 GUI";
                CoreCommandResult r2 = await client.RunCommandAsync(new[] { "write", imgPath, "/hello.txt", helloText });
                AssertEqual(0, r2.ExitCode, "write root file exit");

                CoreCommandResult r3 = await client.RunCommandAsync(new[] { "write", imgPath, "/ドキュメント/日本語.txt", "テスト内容" });
                AssertEqual(0, r3.ExitCode, "write nested jp file exit");

                ScanResponse inspect = await client.InspectSourceAsync(imgPath);
                AssertEqual(1, inspect.Disks.Count, "inspect disk count");
                AssertEqual(1, inspect.Disks[0].Partitions.Count, "inspect part count");
                PartitionInfo p = inspect.Disks[0].Partitions[0];
                Assert(p.IsExtFs, "created image must be IsExtFs");
                AssertEqual("ext4", p.FsKind, "FsKind");
                AssertEqual("テストEXT4", p.VolumeLabel, "VolumeLabel");
                Assert(!string.IsNullOrWhiteSpace(p.ExpectedOverlayLowerId), "ExpectedOverlayLowerId populated");
                Assert(p.FeatureIncompatNames.Contains("extents"), "inspect returns readable feature_incompat_names");
                Assert(p.MetadataCsumEnabled && p.SbChecksumValid, "inspect validates superblock CRC32C");
                Assert(p.MountAdvice != null && p.MountAdvice.ReadWriteAllowed && p.MountAdvice.ReadOnlyAllowed, "inspect returns mount_advice");

                string doctorUpper = Path.Combine(tmpDir, "doc_upper");
                DoctorResponse doc1 = await client.RunDoctorAsync(imgPath, 0, doctorUpper);
                Assert(doc1.Source != null && doc1.Source.Readable && doc1.Source.PartitionFound, "doctor reads source image");
                Assert(doc1.Overlay != null && doc1.Overlay.Status == "will_create" && doc1.Overlay.CanMount, "doctor detects will_create upperdir");

                Directory.CreateDirectory(doctorUpper);
                File.WriteAllText(Path.Combine(doctorUpper, "existing.txt"), "unmarked");
                DoctorResponse doc2 = await client.RunDoctorAsync(imgPath, 0, doctorUpper);
                Assert(doc2.Overlay != null && doc2.Overlay.Status == "non_empty_unmarked" && !doc2.Overlay.CanMount, "doctor refuses non-empty unmarked upperdir");

                await client.CreateImageAsync(imgLongLabelPath, 16, "テストラベル長すぎ");
                ScanResponse inspectLong = await client.InspectSourceAsync(imgLongLabelPath);
                string truncatedLabel = inspectLong.Disks[0].Partitions[0].VolumeLabel;
                Assert(!truncatedLabel.Contains('\uFFFD'), "Truncated UTF-8 label must not contain replacement char");
                Assert(System.Text.Encoding.UTF8.GetByteCount(truncatedLabel) <= 16, "Truncated label must fit in 16 bytes");
                AssertEqual("テストラベ", truncatedLabel, "UTF-8 boundary truncated label");

                LsResponse rootLs = await client.ListDirectoryAsync(imgPath, "/", 0);
                Assert(rootLs.Entries.Any(e => e.Name == "ドキュメント" && e.IsDir), "root contains 日本語 dir");
                Assert(rootLs.Entries.Any(e => e.Name == "hello.txt" && !e.IsDir && e.Size == (ulong)System.Text.Encoding.UTF8.GetByteCount(helloText)), "root contains hello.txt");

                LsResponse subLs = await client.ListDirectoryAsync(imgPath, "/ドキュメント", 0);
                LsEntry? jpFile = subLs.Entries.FirstOrDefault(e => e.Name == "日本語.txt");
                Assert(jpFile != null && !jpFile.IsDir && jpFile.Size > 0, "subdir contains 日本語.txt");

                // Phase 2 live CLI overlay status/diff/validate/export/reset round-trip
                string ovUpper = Path.Combine(tmpDir, "cli_ov_upper");
                Directory.CreateDirectory(ovUpper);
                string markerV2 = p.GetExpectedOverlayLowerIdV2(caseSensitive: false) ?? string.Empty;
                Assert(!string.IsNullOrWhiteSpace(markerV2), "markerV2 populated");
                File.WriteAllText(Path.Combine(ovUpper, ".wh.__ext4_lower_id"), markerV2);
                File.WriteAllText(Path.Combine(ovUpper, "added_from_gui.txt"), "added-body");
                File.WriteAllText(Path.Combine(ovUpper, ".wh.hello.txt"), string.Empty);

                OverlayStatusResponse ovDiff = await client.GetOverlayDiffAsync(ovUpper, imgPath, 0, diff: true);
                AssertEqual(1UL, ovDiff.Summary.AddedCount, "live overlay added count");
                AssertEqual(1UL, ovDiff.Summary.DeletedCount, "live overlay deleted count");
                Assert(ovDiff.Entries.Any(e => e.Kind == "added" && e.Path == "/added_from_gui.txt"), "live overlay added entry");
                Assert(ovDiff.Entries.Any(e => e.Kind == "deleted" && e.Path == "/hello.txt"), "live overlay deleted entry");

                OverlayValidateResponse ovVal = await client.ValidateOverlayAsync(ovUpper, imgPath, 0);
                Assert(ovVal.Valid && ovVal.ErrorCount == 0, "live overlay valid");

                string exportTar = Path.Combine(tmpDir, "overlay_export.tar");
                OverlayExportResponse ovExp = await client.ExportOverlayAsync(ovUpper, exportTar, imgPath, 0);
                Assert(File.Exists(exportTar) && ovExp.ArchiveBytes >= 1536, "live overlay tar exported");

                OverlayResetResponse ovRes = await client.ResetOverlayAsync(ovUpper);
                Assert(ovRes.KeptMarker && ovRes.RemovedEntries >= 2, "live overlay reset preserved marker");
                Assert(File.Exists(Path.Combine(ovUpper, ".wh.__ext4_lower_id")), "marker remains after reset");
                Assert(!File.Exists(Path.Combine(ovUpper, "added_from_gui.txt")), "added file deleted by reset");
            }
            finally
            {
                try { Directory.Delete(tmpDir, recursive: true); } catch { }
            }
        }

        private static void Test_DriveLetterService_SafeUnmountTarget()
        {
            Assert(!DriveLetterService.IsSafeUnmountTarget(null, false, null), "null target rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget("", false, null), "empty target rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\HarddiskVolume2", true, "NTFS"), "physical NTFS rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\HarddiskVolume2", false, null), "dead HarddiskVolume rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\CdRom0", false, null), "CdRom rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\??\C:\mounted", true, "NTFS"), "subst rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\LanmanRedirector\;Z:0\srv\share", true, "NTFS"), "network drive rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\Volume{11111111-2222-3333-4444-555555555555}", true, "NTFS"), "live non-ext4 Volume rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\WinFsp.Disk\rclone", true, "FUSE"), "live non-ext4 WinFsp rejected");
            Assert(!DriveLetterService.IsSafeUnmountTarget(@"\Device\Volume{11111111-2222-3333-4444-555555555555}", true, null), "live unknown fs rejected");

            Assert(DriveLetterService.IsSafeUnmountTarget(@"\Device\Volume{11111111-2222-3333-4444-555555555555}", true, "ext4"), "live ext4 Volume allowed");
            Assert(DriveLetterService.IsSafeUnmountTarget(@"\Device\Volume{11111111-2222-3333-4444-555555555555}", false, null), "dead Volume link allowed");
            Assert(DriveLetterService.IsSafeUnmountTarget(@"\Device\WinFsp.Disk\ext4", false, null), "dead WinFsp link allowed");

            DriveUnmountInspection cDrive = DriveLetterService.InspectDriveForUnmount("C:");
            Assert(cDrive.HasTarget, "C: has target");
            Assert(!cDrive.IsSafeToUnmount, "C: must never be marked safe to unmount");
        }
    }
}
