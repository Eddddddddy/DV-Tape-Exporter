// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic process/state tests: no USB, COM, FFmpeg, or private media access.
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using DvTape;

internal static class LifecycleTests
{
    const int PrefixBytes = 32768;
    static readonly byte[] Payload = Enumerable.Range(0, 131071).Select(i => (byte)((i * 37 + 11) & 255)).ToArray();
    static readonly List<string> Passed = new();
    static readonly List<object> Cases = new();
    static StreamWriter? checkLog;

    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--child") return await Child(args[1], args[2]);
        if (args.Length != 2 || args[0] != "--work-root")
        {
            Console.Error.WriteLine("Usage: dotnet DV.Ui.Lifecycle.Tests.dll --work-root .artifacts/ui-lifecycle-tests");
            return 2;
        }
        string run = Path.Combine(Path.GetFullPath(args[1]), "run-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(run);
        using var log = new StreamWriter(new FileStream(Path.Combine(run, "checks.txt"), FileMode.CreateNew), new UTF8Encoding(false)) { AutoFlush = true };
        checkLog = log;
        string? failure = null;
        try
        {
            TestStates();
            var normal = await RunChild("normal-stop", "cooperative", Path.Combine(run, "normal-stop-synthetic.raw"), run);
            Check(!normal.Forced && normal.ExitCode == 0, "stdin stop exits normally without force kill");
            Check(normal.Done.HasValue && normal.Done.Value.GetProperty("stop_reason").GetString() == "stdin_stop", "helper sends the literal stop line");
            Check(normal.Done.HasValue && normal.Done.Value.GetProperty("stdin_eof").GetBoolean(), "helper closes stdin after stop; child observes EOF");
            Check(normal.Stdout.Any(IsFinishing), "stdout owner drains finishing event while helper waits");
            Check(normal.Stderr.Any(x => x == "stderr-finished"), "stderr owner drains final child diagnostic");
            AssertSource(normal.Path, Payload, "normal stdin stop");
            var saved = Media.AssessCapture(normal.Done, normal.ExitCode, new FileInfo(normal.Path).Length);
            RecordCase("normal-stop", normal, saved);
            Check(saved.Saved && saved.BytesAgree && saved.ZeroLossVerified, "fully drained synthetic capture is eligible for conversion");
            TestNaturalExit(saved.Saved);

            var eof = await RunChild("parent-eof", "cooperative", Path.Combine(run, "parent-eof-synthetic.raw"), run, closeInput: true);
            Check(!eof.Forced && eof.ExitCode == 0, "already closed stdin still waits for normal exit");
            Check(eof.Done.HasValue && eof.Done.Value.GetProperty("stop_reason").GetString() == "parent_eof", "parent pipe EOF independently stops the child");
            AssertSource(eof.Path, Payload, "parent EOF");
            var eofSaved = Media.AssessCapture(eof.Done, eof.ExitCode, new FileInfo(eof.Path).Length);
            RecordCase("parent-eof", eof, eofSaved);
            Check(eofSaved.Saved, "EOF capture has a valid final byte agreement");

            var hung = await RunChild("ignores-input", "hung", Path.Combine(run, "hung-synthetic.raw"), run, timeout: TimeSpan.FromMilliseconds(200));
            Check(hung.Forced && hung.ExitCode != 0, "unresponsive child requires timeout process-tree kill");
            Check(hung.ShutdownMilliseconds < 5000, "shutdown timeout is reachable with stdout still open");
            Check(!hung.Done.HasValue && hung.Stdout.Any(x => x.Contains("heartbeat", StringComparison.Ordinal)), "timeout child has no valid done event while stdout owner keeps draining");
            Check(hung.Stderr.Any(x => x == "stderr-hung"), "stderr owner drains unresponsive child diagnostic");
            AssertSource(hung.Path, Payload[..PrefixBytes], "forced termination");
            var failed = Media.AssessCapture(hung.Done, hung.ExitCode, new FileInfo(hung.Path).Length);
            RecordCase("ignores-input", hung, failed);
            Check(!failed.Saved && !failed.BytesAgree && !failed.PhysicalEndVerified, "force-killed capture cannot be reported saved or physically complete");
            var rejected = CapturingState(); rejected.RequestStop();
            Check(!rejected.BeginConversionAfterCapture(failed.Saved, true, true), "force-killed capture is not eligible for automatic MP4");

            var duplicate = await RunChild("existing-source", "cooperative", normal.Path, run, expectReady: false);
            Check(!duplicate.Forced && duplicate.ExitCode != 0 && !duplicate.Done.HasValue, "child exclusively rejects an existing raw path");
            AssertSource(normal.Path, Payload, "rejected duplicate output");
            var duplicateFailed = Media.AssessCapture(duplicate.Done, duplicate.ExitCode, Payload.Length);
            RecordCase("existing-source", duplicate, duplicateFailed);
            Check(!duplicateFailed.Saved, "existing output failure cannot qualify for conversion");
            Console.WriteLine($"PASS: {Passed.Count} checks; artifacts: {run}");
        }
        catch (Exception ex)
        {
            failure = ex.ToString();
            Console.Error.WriteLine(failure);
            log.WriteLine("FAIL: " + failure);
        }
        using (var summary = new FileStream(Path.Combine(run, "summary.json"), FileMode.CreateNew))
            JsonSerializer.Serialize(summary, new { status = failure == null ? "passed" : "failed", checker_count = Passed.Count, checks = Passed, cases = Cases, failure, synthetic_payload_bytes = Payload.Length, synthetic_prefix_bytes = PrefixBytes, device_access = false, private_media_access = false }, Media.JsonOptions);
        checkLog = null;
        return failure == null ? 0 : 1;
    }

    static void Check(bool value, string name)
    {
        if (!value) throw new InvalidOperationException("FAIL: " + name);
        Passed.Add(name); checkLog?.WriteLine("PASS: " + name); Console.WriteLine("PASS: " + name);
    }

    static OperationState CapturingState()
    {
        var state = new OperationState(); state.Begin(); state.CaptureStarted(); return state;
    }

    static void DispatchCancellation(StopAction action, CancellationTokenSource token)
    {
        if (action == StopAction.CancelTask) token.Cancel();
    }

    static void TestStates()
    {
        var state = new OperationState();
        Check(state.Phase == WorkPhase.Idle && !state.Busy && state.CanUsePrimary, "idle exposes start without an active operation");
        state.Begin(); state.CaptureStarted();
        using var conversion = new CancellationTokenSource();
        var action = state.RequestStop(); DispatchCancellation(action, conversion);
        Check(action == StopAction.StopCapture && state.Phase == WorkPhase.SavingCapture, "manual capture stop requests saving rather than task cancellation");
        Check(!conversion.IsCancellationRequested && !state.CanUsePrimary, "capture stop preserves future conversion token and disables duplicate primary action");
        Check(state.RequestStop() == StopAction.None, "duplicate capture stop is idempotent while saving");
        Check(state.BeginConversionAfterCapture(true, true, true) && state.Phase == WorkPhase.Converting, "manual capture stop can proceed to enabled MP4 conversion");
        Check(state.CanUsePrimary && state.PrimaryText == "取消转换", "conversion presents an independently usable cancel action");
        action = state.RequestStop(); DispatchCancellation(action, conversion);
        Check(action == StopAction.CancelTask && conversion.IsCancellationRequested, "conversion can be cancelled after an earlier capture stop");
        Check(state.RequestStop(closeWindow: true) == StopAction.CancelTask && state.CloseAfterWork, "close during cancelled conversion still latches window-close intent");
        state.End();
        Check(state.Phase == WorkPhase.Idle && !state.Busy, "operation only returns to idle after owner completion");
        state.Begin();
        Check(!state.CloseAfterWork, "new operation clears the prior close intent");
        state.End();

        var closing = CapturingState();
        Check(closing.RequestStop(closeWindow: true) == StopAction.StopCapture && closing.CloseAfterWork, "close during capture requests saving and remembers close intent");
        Check(closing.RequestStop(closeWindow: true) == StopAction.None, "repeated close during saving does not send another capture stop");
        Check(!closing.BeginConversionAfterCapture(true, true, true), "closing after successful capture skips automatic conversion");
        closing.End();
        Check(closing.CloseAfterWork && !closing.Busy, "close intent survives saving completion until the window owner closes");

        var lateClose = CapturingState(); lateClose.RequestStop();
        Check(lateClose.RequestStop(closeWindow: true) == StopAction.None && lateClose.CloseAfterWork, "close after manual capture stop is not blocked by the stop latch");
        Check(!lateClose.BeginConversionAfterCapture(true, true, true), "close arriving during raw finalization prevents a later conversion");
        foreach (var flags in new[] { (Saved: false, Enabled: true, Ready: true), (Saved: true, Enabled: false, Ready: true), (Saved: true, Enabled: true, Ready: false) })
            Check(!CapturingState().BeginConversionAfterCapture(flags.Saved, flags.Enabled, flags.Ready), $"conversion prerequisite enforced: saved={flags.Saved}, enabled={flags.Enabled}, ready={flags.Ready}");
    }

    static void TestNaturalExit(bool saved)
    {
        // The child really exited before this request; the UI has not advanced its phase.
        var state = CapturingState(); using var token = new CancellationTokenSource();
        var action = state.RequestStop(); DispatchCancellation(action, token);
        Check(action == StopAction.StopCapture && !token.IsCancellationRequested, "natural child exit with phase Capturing does not cancel future conversion");
        Check(state.BeginConversionAfterCapture(saved, true, true), "natural-exit stop race remains eligible for conversion after byte checks");
    }

    static void AssertSource(string path, byte[] expected, string label)
    {
        Check(File.Exists(path) && new FileInfo(path).Length == expected.Length, label + " preserves expected raw byte count");
        byte[] bytes = File.ReadAllBytes(path);
        Check(SHA256.HashData(bytes).AsSpan().SequenceEqual(SHA256.HashData(expected)), label + " preserves the expected raw SHA-256");
        using var exclusiveRead = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None);
        Check(exclusiveRead.Length == expected.Length, label + " leaves raw closed and available after child exit");
    }

    sealed record ChildResult(string Path, bool Forced, int ExitCode, long ShutdownMilliseconds, JsonElement? Done, List<string> Stdout, List<string> Stderr);

    static void RecordCase(string name, ChildResult result, CaptureOutcome outcome)
    {
        Cases.Add(new
        {
            case_name = name, raw_file = Path.GetFileName(result.Path),
            actual_bytes = new FileInfo(result.Path).Length,
            sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(result.Path))).ToLowerInvariant(),
            forced_kill = result.Forced, exit_code = result.ExitCode,
            shutdown_ms = result.ShutdownMilliseconds, done = result.Done,
            saved = outcome.Saved, bytes_agree = outcome.BytesAgree,
            physical_end_verified = outcome.PhysicalEndVerified,
            zero_loss_verified = outcome.ZeroLossVerified
        });
    }

    static ProcessStartInfo SelfCommand(string mode, string path)
    {
        string executable = Environment.ProcessPath ?? throw new InvalidOperationException("No current runtime path.");
        var info = Media.Command(executable, Array.Empty<string>());
        if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            info.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
        info.ArgumentList.Add("--child"); info.ArgumentList.Add(mode); info.ArgumentList.Add(path);
        return info;
    }

    static async Task<ChildResult> RunChild(string name, string mode, string raw, string run, bool closeInput = false, bool expectReady = true, TimeSpan? timeout = null)
    {
        using var process = new Process { StartInfo = SelfCommand(mode, raw) };
        process.Start();
        var stdout = new List<string>(); var stderr = new List<string>();
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task output = Drain(process.StandardOutput, stdout, Path.Combine(run, name + "-stdout.txt"), ready);
        Task errors = Drain(process.StandardError, stderr, Path.Combine(run, name + "-stderr.txt"));
        bool forced;
        var watch = new Stopwatch();
        try
        {
            if (expectReady) await ready.Task.WaitAsync(TimeSpan.FromSeconds(5));
            else await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            if (closeInput) process.StandardInput.Close();
            watch.Start();
            forced = await CaptureShutdown.Stop(process, timeout ?? TimeSpan.FromSeconds(3)).WaitAsync(TimeSpan.FromSeconds(5));
            watch.Stop();
            await Task.WhenAll(output, errors).WaitAsync(TimeSpan.FromSeconds(5));
            if (expectReady && mode == "cooperative")
                Check(!await CaptureShutdown.Stop(process, TimeSpan.FromMilliseconds(50)), name + " repeated Stop after child exit is harmless and not forced");
        }
        finally
        {
            if (!process.HasExited) { process.Kill(entireProcessTree: true); await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5)); }
            await Task.WhenAll(output, errors).WaitAsync(TimeSpan.FromSeconds(5));
        }
        JsonElement? done = null;
        foreach (var line in stdout)
        {
            using var document = JsonDocument.Parse(line);
            if (document.RootElement.GetProperty("type").GetString() == "done")
            {
                if (done.HasValue) throw new InvalidOperationException("Duplicate final child event.");
                done = document.RootElement.Clone();
            }
        }
        return new(raw, forced, process.ExitCode, watch.ElapsedMilliseconds, done, stdout, stderr);
    }

    static async Task Drain(StreamReader input, List<string> lines, string logPath, TaskCompletionSource? ready = null)
    {
        using var log = new StreamWriter(new FileStream(logPath, FileMode.CreateNew), new UTF8Encoding(false));
        while (await input.ReadLineAsync() is { } line)
        {
            lines.Add(line); await log.WriteLineAsync(line);
            if (ready != null)
            {
                using var document = JsonDocument.Parse(line);
                if (document.RootElement.GetProperty("type").GetString() == "ready") ready.TrySetResult();
            }
        }
    }

    static bool IsFinishing(string line)
    {
        using var document = JsonDocument.Parse(line);
        return document.RootElement.GetProperty("type").GetString() == "finishing";
    }

    static async Task<int> Child(string mode, string raw)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        try
        {
            using (var source = new FileStream(raw, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            {
                await source.WriteAsync(Payload.AsMemory(0, PrefixBytes)); source.Flush(flushToDisk: true);
                Console.WriteLine(JsonSerializer.Serialize(new { type = "ready", written = PrefixBytes }));
                Console.Out.Flush();
                if (mode == "hung")
                {
                    Console.Error.WriteLine("stderr-hung"); Console.Error.Flush();
                    while (true)
                    {
                        Console.WriteLine("{\"type\":\"heartbeat\"}"); Console.Out.Flush();
                        await Task.Delay(20);
                    }
                }
                string? command;
                do { command = await Console.In.ReadLineAsync(); } while (command != null && command != "stop");
                bool eof = command == null;
                if (!eof) eof = await Console.In.ReadLineAsync() == null;
                Console.WriteLine("{\"type\":\"finishing\"}"); Console.Out.Flush();
                // Delay after stop to prove the helper waits for the writer to drain.
                await Task.Delay(80);
                await source.WriteAsync(Payload.AsMemory(PrefixBytes)); source.Flush(flushToDisk: true);
                source.Dispose();
                Console.Error.WriteLine("stderr-finished"); Console.Error.Flush();
                Console.WriteLine(JsonSerializer.Serialize(new { type = "done", bytes = Payload.Length, written = Payload.Length, drop = 0, ovf = 0, write_error = 0, physical_end_verified = false, stdin_eof = eof, stop_reason = command == null ? "parent_eof" : "stdin_stop" }));
                Console.Out.Flush();
            }
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.GetType().Name + ": synthetic child could not complete its exclusive output.");
            return 17;
        }
    }
}
