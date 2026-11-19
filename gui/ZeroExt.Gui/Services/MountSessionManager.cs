using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using ZeroExt.Gui.Models;

namespace ZeroExt.Gui.Services
{
    public sealed class MountFailedException : Exception
    {
        public TranslatedMountError Translated { get; }

        public MountFailedException(TranslatedMountError translated)
            : base(translated.Message)
        {
            Translated = translated;
        }
    }

    public sealed class MountSessionManager : IDisposable
    {
        private sealed class MountSession
        {
            public required ActiveMountInfo Info { get; init; }
            public required Process Process { get; init; }
            public required EventWaitHandle StopEvent { get; init; }
            public List<string> StdErrLines { get; } = new List<string>();
            public MountEventMessage? LastErrorEvent { get; set; }
            public bool StopRequested { get; set; }
            public ulong LastStatReadBytes { get; set; }
            public ulong LastStatWriteBytes { get; set; }
            public DateTime LastStatTimeUtc { get; set; } = DateTime.UtcNow;
        }

        private readonly Func<string?> _coreExeProvider;
        private readonly Action<string>? _logSink;
        private readonly Action<Action> _marshalToUi;
        private readonly Dictionary<string, MountSession> _sessions = new Dictionary<string, MountSession>();
        private readonly object _gate = new object();

        public ObservableCollection<ActiveMountInfo> ActiveMounts { get; } = new ObservableCollection<ActiveMountInfo>();

        public MountSessionManager(
            Func<string?> coreExeProvider,
            Action<string>? logSink = null,
            Action<Action>? marshalToUi = null)
        {
            _coreExeProvider = coreExeProvider ?? throw new ArgumentNullException(nameof(coreExeProvider));
            _logSink = logSink;
            if (marshalToUi != null)
            {
                _marshalToUi = marshalToUi;
            }
            else
            {
                SynchronizationContext? ctx = SynchronizationContext.Current;
                _marshalToUi = ctx != null
                    ? (act => ctx.Post(_ => act(), null))
                    : (act => act());
            }
        }

        public async Task<ActiveMountInfo> StartMountAsync(MountRequest req, CancellationToken ct = default)
        {
            if (req == null) throw new ArgumentNullException(nameof(req));

            string? exe = _coreExeProvider();
            if (string.IsNullOrWhiteSpace(exe))
            {
                throw new MountFailedException(new TranslatedMountError
                {
                    Title = "zeroext.exe が見つかりません",
                    Message = "マウントエンジンである zeroext.exe を検出できませんでした。",
                    Suggestion = "build\\zeroext.exe をビルドするか、詳細タブからパスを確認してください。",
                });
            }

            string sessionId = Guid.NewGuid().ToString("N").Substring(0, 12);
            string stopEventName = $"Local\\ZeroExtStop_{sessionId}";
            var stopEvent = new EventWaitHandle(false, EventResetMode.ManualReset, stopEventName);

            List<string> args = ZeroExtArgBuilder.BuildMountArgs(
                req,
                stopEventName: stopEventName,
                jsonEvents: true,
                statsIntervalMs: 1000);

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

            var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            if (!proc.Start())
            {
                stopEvent.Dispose();
                proc.Dispose();
                throw new MountFailedException(new TranslatedMountError
                {
                    Title = "プロセス起動エラー",
                    Message = $"zeroext.exe の起動に失敗しました: {exe}",
                    Suggestion = "実行ファイルへのアクセス権限を確認してください。",
                });
            }

            var stderrLines = new List<string>();
            MountEventMessage? lastErrorEvent = null;
            var readyTcs = new TaskCompletionSource<ActiveMountInfo>(TaskCreationOptions.RunContinuationsAsynchronously);
            MountSession? session = null;

            _logSink?.Invoke($"[mount:{req.MountPoint}] 起動 PID={proc.Id}: {ZeroExtArgBuilder.BuildMountPreviewCommand(req)}");

            Task stderrTask = Task.Run(async () =>
            {
                try
                {
                    while (true)
                    {
                        string? line = await proc.StandardError.ReadLineAsync().ConfigureAwait(false);
                        if (line == null) break;
                        lock (stderrLines)
                        {
                            if (stderrLines.Count < 200) stderrLines.Add(line);
                        }
                        if (session != null)
                        {
                            lock (session.StdErrLines)
                            {
                                if (session.StdErrLines.Count < 200) session.StdErrLines.Add(line);
                            }
                        }
                        _logSink?.Invoke($"[{req.MountPoint}] {line}");
                    }
                }
                catch
                {
                    // Stream closed
                }
            });

            Task stdoutTask = Task.Run(async () =>
            {
                try
                {
                    while (true)
                    {
                        string? line = await proc.StandardOutput.ReadLineAsync().ConfigureAwait(false);
                        if (line == null) break;

                        MountEventMessage? ev = ZeroExtCoreClient.TryParseMountEvent(line);
                        if (ev == null)
                        {
                            _logSink?.Invoke($"[{req.MountPoint}] {line}");
                            continue;
                        }

                        if (string.Equals(ev.Event, "mounted", StringComparison.Ordinal))
                        {
                            var info = new ActiveMountInfo
                            {
                                Id = sessionId,
                                SourcePath = ev.Source ?? req.SourcePath,
                                PartitionNumber = ev.Partition ?? req.PartitionNumber,
                                MountPoint = ZeroExtArgBuilder.NormalizeMountPointArg(ev.MountPoint ?? req.MountPoint),
                                VolumeLabel = !string.IsNullOrWhiteSpace(ev.VolumeLabel) ? ev.VolumeLabel : req.VolumeLabel,
                                ModeDisplay = ev.Mode ?? req.Mode.ToString(),
                                IsReadOnly = ev.ReadOnly ?? (req.Mode == MountMode.ReadOnly),
                                IsOverlay = req.Mode == MountMode.Overlay ||
                                            (!string.IsNullOrEmpty(ev.OverlayUpper)),
                                OverlayUpperDir = ev.OverlayUpper ?? req.OverlayUpperDir,
                                DirectIoEnabled = ev.DirectIo ?? req.EnableDirectIo,
                                FellBackToRo = ev.FellBackToRo ?? false,
                                ProcessId = proc.Id,
                                StartedAt = DateTime.Now,
                                StatusText = (ev.FellBackToRo ?? false) ? "RO フォールバック中" : "マウント中",
                            };

                            session = new MountSession
                            {
                                Info = info,
                                Process = proc,
                                StopEvent = stopEvent,
                                LastErrorEvent = lastErrorEvent,
                                LastStatTimeUtc = DateTime.UtcNow,
                            };

                            lock (_gate)
                            {
                                _sessions[sessionId] = session;
                            }

                            _marshalToUi(() => ActiveMounts.Add(info));
                            _logSink?.Invoke($"[{info.MountPoint}] マウント完了: mode={info.ModeDisplay}, direct_io={(info.DirectIoEnabled ? "on" : "off")}");
                            readyTcs.TrySetResult(info);
                        }
                        else if (string.Equals(ev.Event, "stats", StringComparison.Ordinal) && session != null)
                        {
                            ulong rBytes = (ulong)Math.Max(0L, ev.ReadBytes ?? 0L);
                            ulong wBytes = (ulong)Math.Max(0L, ev.WriteBytes ?? 0L);
                            long rOps = ev.ReadOps ?? 0L;
                            long wOps = ev.WriteOps ?? 0L;
                            DateTime nowUtc = DateTime.UtcNow;
                            double dt = Math.Max(0.1, (nowUtc - session.LastStatTimeUtc).TotalSeconds);
                            double rRate = rBytes >= session.LastStatReadBytes ? (rBytes - session.LastStatReadBytes) / dt : 0.0;
                            double wRate = wBytes >= session.LastStatWriteBytes ? (wBytes - session.LastStatWriteBytes) / dt : 0.0;
                            session.LastStatReadBytes = rBytes;
                            session.LastStatWriteBytes = wBytes;
                            session.LastStatTimeUtc = nowUtc;

                            ActiveMountInfo targetInfo = session.Info;
                            _marshalToUi(() =>
                            {
                                targetInfo.ReadBytes = rBytes;
                                targetInfo.WriteBytes = wBytes;
                                targetInfo.ReadOps = rOps;
                                targetInfo.WriteOps = wOps;
                                targetInfo.ReadBytesPerSec = rRate;
                                targetInfo.WriteBytesPerSec = wRate;
                            });
                        }
                        else if (string.Equals(ev.Event, "unmounting", StringComparison.Ordinal) && session != null)
                        {
                            ActiveMountInfo targetInfo = session.Info;
                            _marshalToUi(() =>
                            {
                                targetInfo.IsUnmounting = true;
                                targetInfo.StatusText = "アンマウント中...";
                            });
                            _logSink?.Invoke($"[{targetInfo.MountPoint}] アンマウント開始...");
                        }
                        else if (string.Equals(ev.Event, "unmounted", StringComparison.Ordinal) && session != null)
                        {
                            _logSink?.Invoke($"[{session.Info.MountPoint}] 正常にアンマウントされました。");
                        }
                        else if (string.Equals(ev.Event, "error", StringComparison.Ordinal))
                        {
                            lastErrorEvent = ev;
                            if (session != null)
                            {
                                session.LastErrorEvent = ev;
                            }
                            _logSink?.Invoke($"[{req.MountPoint}] ERROR({ev.Stage}): {ev.Message}");
                        }
                    }
                }
                catch
                {
                    // Stream closed
                }
            });

            _ = Task.Run(async () =>
            {
                try
                {
                    await proc.WaitForExitAsync().ConfigureAwait(false);
                }
                catch
                {
                    // Ignore
                }
                await Task.WhenAll(stdoutTask, stderrTask).ConfigureAwait(false);

                int exitCode = 1;
                try { exitCode = proc.ExitCode; } catch { }

                if (!readyTcs.Task.IsCompleted)
                {
                    List<string> errsCopy;
                    lock (stderrLines)
                    {
                        errsCopy = new List<string>(stderrLines);
                    }
                    TranslatedMountError translated = MountErrorTranslator.Translate(lastErrorEvent, errsCopy, exitCode);
                    stopEvent.Dispose();
                    proc.Dispose();
                    readyTcs.TrySetException(new MountFailedException(translated));
                    return;
                }

                if (session != null)
                {
                    bool expectedStop = session.StopRequested;
                    lock (_gate)
                    {
                        _sessions.Remove(sessionId);
                    }
                    _marshalToUi(() => ActiveMounts.Remove(session.Info));
                    if (!expectedStop)
                    {
                        _logSink?.Invoke($"[{session.Info.MountPoint}] 警告: マウントプロセスが終了しました (exit={exitCode})。");
                    }
                    session.StopEvent.Dispose();
                    session.Process.Dispose();
                }
            });

            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeoutCts.CancelAfter(TimeSpan.FromSeconds(15));
            try
            {
                return await readyTcs.Task.WaitAsync(timeoutCts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
                try { stopEvent.Set(); } catch { }
                try
                {
                    if (!proc.HasExited)
                    {
                        proc.Kill(entireProcessTree: true);
                    }
                }
                catch { }
                throw new MountFailedException(new TranslatedMountError
                {
                    Title = "マウント待機がタイムアウトしました",
                    Message = "zeroext.exe からのマウント完了応答が 15 秒以内に返りませんでした。",
                    Suggestion = "「詳細 & ログ」タブで出力を確認し、必要に応じて「ツール → 残留ドライブ解除」をお試しください。",
                });
            }
        }

        public async Task<bool> UnmountAsync(ActiveMountInfo mount, TimeSpan? timeout = null)
        {
            MountSession? session;
            lock (_gate)
            {
                _sessions.TryGetValue(mount.Id, out session);
            }

            if (session == null)
            {
                _marshalToUi(() => ActiveMounts.Remove(mount));
                return true;
            }

            session.StopRequested = true;
            _marshalToUi(() =>
            {
                mount.IsUnmounting = true;
                mount.StatusText = "アンマウント中...";
            });

            try
            {
                session.StopEvent.Set();
            }
            catch
            {
                // Ignore if already disposed
            }

            TimeSpan waitTime = timeout ?? TimeSpan.FromSeconds(8);
            using var cts = new CancellationTokenSource(waitTime);
            try
            {
                await session.Process.WaitForExitAsync(cts.Token).ConfigureAwait(false);
                return true;
            }
            catch (OperationCanceledException)
            {
                return false;
            }
            catch
            {
                return session.Process.HasExited;
            }
        }

        public async Task ForceKillAndCleanupAsync(ActiveMountInfo mount)
        {
            MountSession? session;
            lock (_gate)
            {
                _sessions.TryGetValue(mount.Id, out session);
            }

            if (session != null)
            {
                session.StopRequested = true;
                try
                {
                    if (!session.Process.HasExited)
                    {
                        session.Process.Kill(entireProcessTree: true);
                    }
                }
                catch
                {
                    // Ignore
                }
            }

            if (DriveLetterService.IsDriveLetter(mount.MountPoint))
            {
                try
                {
                    var cli = new ZeroExtCoreClient(_coreExeProvider, _logSink);
                    await cli.ForceUnmountAsync(mount.MountPoint).ConfigureAwait(false);
                }
                catch
                {
                    // Best effort
                }
            }

            lock (_gate)
            {
                _sessions.Remove(mount.Id);
            }
            _marshalToUi(() => ActiveMounts.Remove(mount));
        }

        public async Task UnmountAllAsync()
        {
            List<ActiveMountInfo> snapshot;
            lock (_gate)
            {
                snapshot = _sessions.Values.Select(s => s.Info).ToList();
            }
            if (snapshot.Count == 0) return;

            var tasks = snapshot.Select(m => UnmountAsync(m, TimeSpan.FromSeconds(6))).ToList();
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }

        public void Dispose()
        {
            List<MountSession> snapshot;
            lock (_gate)
            {
                snapshot = _sessions.Values.ToList();
                _sessions.Clear();
            }
            foreach (MountSession s in snapshot)
            {
                try { s.StopRequested = true; s.StopEvent.Set(); } catch { }
            }
        }
    }
}
