using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using ZeroExt4.Gui.Models;

namespace ZeroExt4.Gui.Services
{
    public sealed class CoreCommandResult
    {
        public int ExitCode { get; init; }
        public string StdOut { get; init; } = string.Empty;
        public string StdErr { get; init; } = string.Empty;
    }

    public sealed class ZeroExt4CoreClient
    {
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
        };

        private readonly Func<string?> _coreExeProvider;
        private readonly Action<string>? _logSink;

        public ZeroExt4CoreClient(Func<string?> coreExeProvider, Action<string>? logSink = null)
        {
            _coreExeProvider = coreExeProvider ?? throw new ArgumentNullException(nameof(coreExeProvider));
            _logSink = logSink;
        }

        public async Task<ScanResponse> ScanPhysicalDisksAsync(CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildListArgs(sourcePath: null, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreScanFailFmt", res.ExitCode, res.StdErr).Trim());
            }
            return ParseScanResponse(res.StdOut);
        }

        public async Task<ScanResponse> InspectSourceAsync(
            string sourcePath,
            uint? partitionNumber = null,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildInspectArgs(sourcePath, partitionNumber, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreInspectFailFmt", sourcePath, res.ExitCode, res.StdErr).Trim());
            }
            return ParseScanResponse(res.StdOut);
        }

        public async Task<FsckLiteReport> RunFsckLiteAsync(
            string sourcePath,
            uint? partitionNumber = null,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildFsckLiteArgs(sourcePath, partitionNumber, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(res.StdOut))
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? $"exit={res.ExitCode}" : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreInspectFailFmt", sourcePath, res.ExitCode, detail).Trim());
            }
            return ParseFsckLiteReport(res.StdOut);
        }

        public async Task<LsResponse> ListDirectoryAsync(
            string sourcePath,
            string ext4Path,
            uint partitionNumber,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildLsArgs(sourcePath, ext4Path, partitionNumber, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreLsFailFmt", ext4Path, res.ExitCode, res.StdErr).Trim());
            }
            return ParseLsResponse(res.StdOut);
        }

        public async Task CreateImageAsync(
            string imagePath,
            uint sizeMb,
            string? label,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildMkfsArgs(imagePath, sizeMb, label);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0)
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? res.StdOut : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreMkfsFailFmt", res.ExitCode, detail).Trim());
            }
        }

        public async Task<CoreCommandResult> ForceUnmountAsync(string driveLetter, CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildUnmountArgs(driveLetter);
            return await RunCommandAsync(args, ct).ConfigureAwait(false);
        }

        public async Task<DoctorResponse> RunDoctorAsync(
            string? sourcePath = null,
            uint? partitionNumber = null,
            string? overlayDir = null,
            bool caseSensitive = false,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildDoctorArgs(
                sourcePath, partitionNumber, overlayDir, caseSensitive, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(res.StdOut))
            {
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreDoctorFailFmt", res.ExitCode, res.StdErr).Trim());
            }
            return ParseDoctorResponse(res.StdOut);
        }

        public async Task<OverlayStatusResponse> GetOverlayDiffAsync(
            string upperDir,
            string? lowerSource = null,
            uint? partitionNumber = null,
            bool caseSensitive = false,
            bool diff = true,
            uint? createUid = null,
            uint? createGid = null,
            bool inheritOwner = false,
            string? fmodeOctal = null,
            string? dmodeOctal = null,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildOverlayStatusArgs(
                upperDir, lowerSource, partitionNumber, caseSensitive, diff, json: true,
                createUid, createGid, inheritOwner, fmodeOctal, dmodeOctal);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? res.StdOut : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreOverlayFailFmt", res.ExitCode, detail).Trim());
            }
            return ParseOverlayStatusResponse(res.StdOut);
        }

        public async Task<OverlayValidateResponse> ValidateOverlayAsync(
            string upperDir,
            string? lowerSource = null,
            uint? partitionNumber = null,
            bool caseSensitive = false,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildOverlayValidateArgs(
                upperDir, lowerSource, partitionNumber, caseSensitive, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (string.IsNullOrWhiteSpace(res.StdOut))
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? res.StdOut : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreOverlayFailFmt", res.ExitCode, detail).Trim());
            }
            return ParseOverlayValidateResponse(res.StdOut);
        }

        public async Task<OverlayExportResponse> ExportOverlayAsync(
            string upperDir,
            string outputTarPath,
            string? lowerSource = null,
            uint? partitionNumber = null,
            bool caseSensitive = false,
            uint? createUid = null,
            uint? createGid = null,
            bool inheritOwner = false,
            string? fmodeOctal = null,
            string? dmodeOctal = null,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildOverlayExportArgs(
                upperDir, outputTarPath, lowerSource, partitionNumber, caseSensitive, json: true,
                createUid, createGid, inheritOwner, fmodeOctal, dmodeOctal);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? res.StdOut : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreOverlayFailFmt", res.ExitCode, detail).Trim());
            }
            return ParseOverlayExportResponse(res.StdOut);
        }

        public async Task<OverlayResetResponse> ResetOverlayAsync(
            string upperDir,
            CancellationToken ct = default)
        {
            List<string> args = ZeroExt4ArgBuilder.BuildOverlayResetArgs(upperDir, yes: true, json: true);
            CoreCommandResult res = await RunCommandAsync(args, ct).ConfigureAwait(false);
            if (res.ExitCode != 0 || string.IsNullOrWhiteSpace(res.StdOut))
            {
                string detail = string.IsNullOrWhiteSpace(res.StdErr) ? res.StdOut : res.StdErr;
                throw new InvalidOperationException(
                    LocalizationService.Format("Str_CoreOverlayFailFmt", res.ExitCode, detail).Trim());
            }
            return ParseOverlayResetResponse(res.StdOut);
        }

        public async Task<CoreCommandResult> RunCommandAsync(
            IReadOnlyList<string> args,
            CancellationToken ct = default)
        {
            string? exe = _coreExeProvider();
            if (string.IsNullOrWhiteSpace(exe))
            {
                throw new InvalidOperationException(LocalizationService.Get("Str_ValNoCoreExe"));
            }

            var psi = new ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            foreach (string arg in args)
            {
                psi.ArgumentList.Add(arg);
            }

            using var proc = new Process { StartInfo = psi };
            if (!proc.Start())
            {
                throw new InvalidOperationException(LocalizationService.Format("Str_CoreStartFailFmt", exe));
            }

            Task<string> outTask = proc.StandardOutput.ReadToEndAsync(ct);
            Task<string> errTask = proc.StandardError.ReadToEndAsync(ct);

            await proc.WaitForExitAsync(ct).ConfigureAwait(false);
            string stdout = await outTask.ConfigureAwait(false);
            string stderr = await errTask.ConfigureAwait(false);

            if (!string.IsNullOrWhiteSpace(stderr))
            {
                foreach (string line in stderr.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries))
                {
                    _logSink?.Invoke($"[cli:err] {line}");
                }
            }

            return new CoreCommandResult
            {
                ExitCode = proc.ExitCode,
                StdOut = stdout,
                StdErr = stderr,
            };
        }

        public static ScanResponse ParseScanResponse(string json)
        {
            ScanResponse? parsed = JsonSerializer.Deserialize<ScanResponse>(json, JsonOptions);
            return parsed ?? new ScanResponse();
        }

        public static LsResponse ParseLsResponse(string json)
        {
            LsResponse? parsed = JsonSerializer.Deserialize<LsResponse>(json, JsonOptions);
            return parsed ?? new LsResponse();
        }

        public static DoctorResponse ParseDoctorResponse(string json)
        {
            DoctorResponse? parsed = JsonSerializer.Deserialize<DoctorResponse>(json, JsonOptions);
            return parsed ?? new DoctorResponse();
        }

        public static OverlayStatusResponse ParseOverlayStatusResponse(string json)
        {
            OverlayStatusResponse? parsed = JsonSerializer.Deserialize<OverlayStatusResponse>(json, JsonOptions);
            return parsed ?? new OverlayStatusResponse();
        }

        public static OverlayValidateResponse ParseOverlayValidateResponse(string json)
        {
            OverlayValidateResponse? parsed = JsonSerializer.Deserialize<OverlayValidateResponse>(json, JsonOptions);
            return parsed ?? new OverlayValidateResponse();
        }

        public static OverlayExportResponse ParseOverlayExportResponse(string json)
        {
            OverlayExportResponse? parsed = JsonSerializer.Deserialize<OverlayExportResponse>(json, JsonOptions);
            return parsed ?? new OverlayExportResponse();
        }

        public static OverlayResetResponse ParseOverlayResetResponse(string json)
        {
            OverlayResetResponse? parsed = JsonSerializer.Deserialize<OverlayResetResponse>(json, JsonOptions);
            return parsed ?? new OverlayResetResponse();
        }

        public static FsckLiteReport ParseFsckLiteReport(string json)
        {
            FsckLiteReport? parsed = JsonSerializer.Deserialize<FsckLiteReport>(json, JsonOptions);
            return parsed ?? new FsckLiteReport();
        }

        public static MountEventMessage? TryParseMountEvent(string line)
        {
            if (string.IsNullOrWhiteSpace(line)) return null;
            string trimmed = line.Trim();
            if (!trimmed.StartsWith('{')) return null;
            try
            {
                return JsonSerializer.Deserialize<MountEventMessage>(trimmed, JsonOptions);
            }
            catch
            {
                return null;
            }
        }
    }
}
