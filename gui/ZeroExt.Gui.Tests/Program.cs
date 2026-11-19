using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Threading.Tasks;
using ZeroExt.Gui.Models;
using ZeroExt.Gui.Services;
using ZeroExt.Gui.ViewModels;

namespace ZeroExt.Gui.Tests
{
    internal static class Program
    {
        private static int _passed;
        private static int _failed;

        private static async Task<int> Main(string[] args)
        {
            Console.OutputEncoding = System.Text.Encoding.UTF8;
            Console.WriteLine("Running ZeroExt.Gui automated tests...");

            Run("JsonParsing_ScanResponse", Test_JsonParsing_ScanResponse);
            Run("JsonParsing_LsAndMountEvents", Test_JsonParsing_LsAndMountEvents);
            Run("ArgBuilder_MountVariantsAndPreview", Test_ArgBuilder_MountVariantsAndPreview);
            Run("DriveLetterAndOverlayValidation", Test_DriveLetterAndOverlayValidation);
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
                Console.WriteLine($"[FAIL] {name}: {ex.Message}");
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
                Console.WriteLine($"[FAIL] {name}: {ex.Message}");
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
                      "fs_kind": "ext4",
                      "journal_state": "NEEDS_RECOVERY",
                      "volume_label": "テスト領域",
                      "uuid": "11223344-5566-4777-8888-99aabbccddee",
                      "block_size": 4096,
                      "inode_size": 256,
                      "blocks_count": 131072,
                      "total_bytes": 536870912,
                      "free_bytes": 134217728
                    }
                  ]
                }
              ]
            }
            """;

            ScanResponse res = ZeroExtCoreClient.ParseScanResponse(json);
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
            AssertEqual(75.0, Math.Round(p1.UsedPercent, 2), "UsedPercent");
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
            LsResponse ls = ZeroExtCoreClient.ParseLsResponse(lsJson);
            AssertEqual("/資料", ls.Path, "LsResponse.Path");
            AssertEqual(2, ls.Entries.Count, "LsResponse.Entries.Count");
            Assert(ls.Entries[0].IsDir, "first entry should be dir");
            AssertEqual("2.00 KB", ls.Entries[1].SizeFormatted, "SizeFormatted");

            MountEventMessage? mounted = ZeroExtCoreClient.TryParseMountEvent(
                "{\"event\":\"mounted\",\"source\":\"test.img\",\"partition\":0,\"mountpoint\":\"M:\",\"mode\":\"RO\",\"read_only\":true,\"direct_io\":true,\"fell_back_to_ro\":false}");
            Assert(mounted != null && mounted.Event == "mounted", "mounted event parsed");
            AssertEqual(true, mounted!.DirectIo ?? false, "DirectIo");

            MountEventMessage? stats = ZeroExtCoreClient.TryParseMountEvent(
                "{\"event\":\"stats\",\"read_bytes\":1048576,\"write_bytes\":4096,\"read_ops\":10,\"write_ops\":2}");
            Assert(stats != null && stats.ReadBytes == 1048576 && stats.WriteOps == 2, "stats event parsed");
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
            };
            List<string> roArgs = ZeroExtArgBuilder.BuildMountArgs(roReq, "Local\\Evt1", true, 1000);
            Assert(!roArgs.Contains("--rw"), "RO should not have --rw");
            Assert(!roArgs.Contains("--no-direct-io"), "RO with DirectIO on should not pass --no-direct-io");
            Assert(roArgs.Contains("--stop-event") && roArgs.Contains("Local\\Evt1"), "stop-event present");
            Assert(roArgs.Contains("--json-events"), "json-events present");
            Assert(roArgs.Contains("--stats-interval") && roArgs.Contains("1000"), "stats-interval present");
            AssertEqual("M:", roArgs[roArgs.IndexOf("--mountpoint") + 1], "normalized drive letter");

            string preview = ZeroExtArgBuilder.BuildMountPreviewCommand(roReq);
            Assert(preview.Contains("\"C:\\my images\\disk.img\""), "preview should quote space paths");
            Assert(!preview.Contains("--stop-event"), "preview should omit internal --stop-event");
            Assert(!preview.Contains("--json-events"), "preview should omit internal --json-events");

            roReq.EnableDirectIo = false;
            List<string> roNoDirect = ZeroExtArgBuilder.BuildMountArgs(roReq);
            Assert(roNoDirect.Contains("--no-direct-io"), "RO with DirectIO off must pass --no-direct-io");

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
            List<string> ovArgs = ZeroExtArgBuilder.BuildMountArgs(ovReq);
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
            List<string> rwArgs = ZeroExtArgBuilder.BuildMountArgs(rwReq);
            Assert(rwArgs.Contains("--rw"), "RW must include --rw");
            Assert(rwArgs.Contains("--ro-fallback"), "RW with fallback must include --ro-fallback");
            Assert(!rwArgs.Contains("--direct-io") && !rwArgs.Contains("--no-direct-io"), "RW must not pass direct-io flags");
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

            string rootDir = Path.Combine(Path.GetTempPath(), $"zeroext_gui_test_{Guid.NewGuid():N}");
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

                string unmarkedUpper = Path.Combine(rootDir, "upper_unmarked");
                Directory.CreateDirectory(unmarkedUpper);
                File.WriteAllText(Path.Combine(unmarkedUpper, "existing.txt"), "hi");
                UpperDirCheckResult r5 = DriveLetterService.CheckOverlayUpperDir(unmarkedUpper, "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.NonEmptyUnmarked, r5.Status, "non-empty unmarked");
                Assert(r5.CanMount, "NonEmptyUnmarked warns but allows mount");

                UpperDirCheckResult r6 = DriveLetterService.CheckOverlayUpperDir("C:\\", "uuid1:100:4096");
                AssertEqual(UpperDirCheckStatus.InvalidPath, r6.Status, "drive root rejected");
                Assert(!r6.CanMount, "drive root must block mount");
            }
            finally
            {
                try { Directory.Delete(rootDir, recursive: true); } catch { }
            }
        }

        private static void Test_MountErrorTranslator()
        {
            var e1 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -22, Message = "Failed" },
                new[] { "[zeroext:ERROR] Overlay upperdir 'D:\\u' belongs to a different lower ext4 filesystem" });
            Assert(e1.Title.Contains("一致しません"), "overlay mismatch error translated");

            var e2 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -30, Message = "EROFS" },
                Array.Empty<string>());
            Assert(e2.Title.Contains("Dirty"), "dirty journal error translated");

            var e3 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -19, Message = "WinFsp mount failed" },
                Array.Empty<string>());
            Assert(e3.Title.Contains("WinFsp"), "winfsp missing error translated");

            var e4 = MountErrorTranslator.Translate(
                new MountEventMessage { Event = "error", Code = -5, Message = "FspFileSystemSetMountPoint failed: NTSTATUS=0xC0000022" },
                Array.Empty<string>());
            Assert(e4.Title.Contains("マウント先"), "0xC0000022 error translated");
        }

        private static void Test_SettingsService_RoundTrip()
        {
            string tmp = Path.Combine(Path.GetTempPath(), $"zeroext_settings_{Guid.NewGuid():N}.json");
            try
            {
                var svc = new SettingsService(tmp);
                var s = new GuiSettings
                {
                    Theme = "Dark",
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
            string tmpSettings = Path.Combine(Path.GetTempPath(), $"zeroext_vm_{Guid.NewGuid():N}.json");
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
                vm.SelectedMode = MountMode.ReadWrite;
                Assert(vm.ShowDirtyRwWarning, "ShowDirtyRwWarning visible in RW mode");
                Assert(!vm.DirectIoEditable, "DirectIO disabled in RW mode");
                Assert(!vm.EffectiveDirectIo, "EffectiveDirectIo false in RW mode");
                Assert(vm.CliPreviewCommand.Contains("--rw"), "CLI preview shows --rw");

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
            Assert(!string.IsNullOrWhiteSpace(exe), "zeroext.exe must be found for integration test");

            string tmpDir = Path.Combine(Path.GetTempPath(), $"zeroext_gui_int_{Guid.NewGuid():N}");
            Directory.CreateDirectory(tmpDir);
            string imgPath = Path.Combine(tmpDir, "統合テスト.img");
            string imgLongLabelPath = Path.Combine(tmpDir, "long_label.img");

            try
            {
                var client = new ZeroExtCoreClient(() => exe);
                await client.CreateImageAsync(imgPath, 16, "テストEXT4");

                CoreCommandResult r1 = await client.RunCommandAsync(new[] { "mkdir", imgPath, "/ドキュメント" });
                AssertEqual(0, r1.ExitCode, "mkdir exit");

                CoreCommandResult r2 = await client.RunCommandAsync(new[] { "write", imgPath, "/hello.txt", "Hello ZeroExt GUI" });
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

                await client.CreateImageAsync(imgLongLabelPath, 16, "テストラベル長すぎ");
                ScanResponse inspectLong = await client.InspectSourceAsync(imgLongLabelPath);
                string truncatedLabel = inspectLong.Disks[0].Partitions[0].VolumeLabel;
                Assert(!truncatedLabel.Contains('\uFFFD'), "Truncated UTF-8 label must not contain replacement char");
                Assert(System.Text.Encoding.UTF8.GetByteCount(truncatedLabel) <= 16, "Truncated label must fit in 16 bytes");
                AssertEqual("テストラベ", truncatedLabel, "UTF-8 boundary truncated label");

                LsResponse rootLs = await client.ListDirectoryAsync(imgPath, "/", 0);
                Assert(rootLs.Entries.Any(e => e.Name == "ドキュメント" && e.IsDir), "root contains 日本語 dir");
                Assert(rootLs.Entries.Any(e => e.Name == "hello.txt" && !e.IsDir && e.Size == 17), "root contains hello.txt");

                LsResponse subLs = await client.ListDirectoryAsync(imgPath, "/ドキュメント", 0);
                LsEntry? jpFile = subLs.Entries.FirstOrDefault(e => e.Name == "日本語.txt");
                Assert(jpFile != null && !jpFile.IsDir && jpFile.Size > 0, "subdir contains 日本語.txt");
            }
            finally
            {
                try { Directory.Delete(tmpDir, recursive: true); } catch { }
            }
        }
    }
}
