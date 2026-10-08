// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace DvDirectShow;

internal sealed class StopRequest
{
    private string? reason;
    public string? Reason => Volatile.Read(ref reason);
    public void Request(string value) => Interlocked.CompareExchange(ref reason, value, null);
}

// The callback only copies data into a bounded queue. Disk IO and JSON stay off
// the DirectShow streaming thread; no sample is decoded, resampled or rewritten.
[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public sealed class RawSampleWriter : ISampleGrabberCB, IDisposable
{
    private readonly record struct Chunk(byte[] Buffer, int Length);
    private readonly Channel<Chunk> queue = Channel.CreateBounded<Chunk>(new BoundedChannelOptions(256)
    {
        SingleReader = true, SingleWriter = false, FullMode = BoundedChannelFullMode.Wait,
        AllowSynchronousContinuations = false
    });
    private const long Budget = 64L * 1024 * 1024;
    private readonly FileStream file;
    private readonly Task worker;
    private long pendingBytes, received, accepted, written, overflows, samples;
    private int accepting = 1, completed;
    private Exception? failure;
    public long Received => Interlocked.Read(ref received);
    public long Accepted => Interlocked.Read(ref accepted);
    public long Written => Interlocked.Read(ref written);
    public long Overflows => Interlocked.Read(ref overflows);
    public long Samples => Interlocked.Read(ref samples);
    public Exception? Failure => Volatile.Read(ref failure);
    public bool WriteError { get; private set; }
    public string OutputPath { get; }

    public RawSampleWriter(string path)
    {
        OutputPath = Path.GetFullPath(path);
        // CREATE_NEW is atomic, accepts Unicode paths, and never truncates a file.
        file = new FileStream(OutputPath, FileMode.CreateNew, FileAccess.Write, FileShare.Read,
            1024 * 1024, FileOptions.SequentialScan);
        worker = Task.Run(WriteLoop);
    }

    public int SampleCB(double time, IntPtr sample) => 0; // BufferCB is selected.
    public int BufferCB(double time, IntPtr buffer, int length)
    {
        if (Volatile.Read(ref accepting) == 0 || Failure is not null) return 0;
        if (length <= 0) return 0;
        Interlocked.Add(ref received, length);
        Interlocked.Increment(ref samples);
        byte[]? copy = null;
        bool budgetOwned = false;
        try
        {
            if (buffer == IntPtr.Zero || length > 16 * 1024 * 1024)
                throw new InvalidDataException("Invalid raw DV sample buffer.");
            long pending = Interlocked.Add(ref pendingBytes, length);
            budgetOwned = true;
            if (pending > Budget)
            {
                Interlocked.Increment(ref overflows);
                throw new IOException("Raw DV output queue reached its 64 MiB limit; the partial file is retained.");
            }
            copy = ArrayPool<byte>.Shared.Rent(length);
            Marshal.Copy(buffer, copy, 0, length);
            if (!queue.Writer.TryWrite(new Chunk(copy, length)))
            {
                Interlocked.Increment(ref overflows);
                throw new IOException("Raw DV output queue was full or closed; the partial file is retained.");
            }
            copy = null; // Writer owns the buffer and pending-byte reservation.
            budgetOwned = false;
            Interlocked.Add(ref accepted, length);
        }
        catch (Exception ex) { Interlocked.CompareExchange(ref failure, ex, null); }
        finally
        {
            if (copy is not null) ArrayPool<byte>.Shared.Return(copy);
            if (budgetOwned) Interlocked.Add(ref pendingBytes, -length);
        }
        return 0; // Main thread stops the graph; never re-enter COM from callback.
    }

    private async Task WriteLoop()
    {
        bool diskFailed = false;
        try
        {
            await foreach (Chunk chunk in queue.Reader.ReadAllAsync())
            {
                try
                {
                    if (!diskFailed)
                    {
                        await file.WriteAsync(chunk.Buffer.AsMemory(0, chunk.Length));
                        Interlocked.Add(ref written, chunk.Length);
                    }
                }
                catch (Exception ex)
                {
                    diskFailed = true;
                    WriteError = true;
                    Interlocked.CompareExchange(ref failure, ex, null);
                }
                finally
                {
                    ArrayPool<byte>.Shared.Return(chunk.Buffer);
                    Interlocked.Add(ref pendingBytes, -chunk.Length);
                }
            }
            if (!diskFailed) file.Flush(flushToDisk: true);
        }
        catch (Exception ex)
        {
            WriteError = true;
            Interlocked.CompareExchange(ref failure, ex, null);
        }
        finally
        {
            try { file.Dispose(); }
            catch (Exception ex)
            {
                WriteError = true;
                Interlocked.CompareExchange(ref failure, ex, null);
            }
        }
    }

    public void Complete()
    {
        if (Interlocked.Exchange(ref completed, 1) != 0) return;
        Volatile.Write(ref accepting, 0);
        queue.Writer.TryComplete();
        worker.GetAwaiter().GetResult();
    }
    public void Dispose() => Complete();
}

internal static class Tape
{
    public static (bool Supported, int? Mode, int? HResult) Read(IBaseFilter source)
    {
        if (source is not IAMExtTransport transport) return (false, null, null);
        int hr = transport.get_Mode(out int mode);
        return hr >= 0 ? (true, mode, hr) : (false, null, hr);
    }
    public static (bool Supported, int? HResult) Set(IBaseFilter source, string action)
    {
        // Deliberately exhaustive playback-only whitelist. No recording mode exists
        // in the CLI mapping, even if a caller supplies an arbitrary number.
        int mode = action switch
        {
            "play" => Ds.ModePlay, "rew" => Ds.ModeRewind, "stop" => Ds.ModeStop,
            _ => throw new ArgumentException("Tape action must be status, play, rew or stop.")
        };
        if (source is not IAMExtTransport transport) return (false, null);
        int hr = transport.put_Mode(mode);
        return (hr >= 0, hr);
    }
    public static string Name(int? mode) => mode switch
    {
        Ds.ModePlay => "play", Ds.ModeStop => "stop", Ds.ModeRewind => "rewind", null => "unknown", _ => "other"
    };
    public static int Command(string id, string action)
    {
        IBaseFilter source = Devices.Open(id);
        try
        {
            using RawPin? raw = Devices.FindRawCapturePin(source);
            if (raw is null) throw new InvalidOperationException("The selected device has no raw dvsd capture pin.");
            bool ok = true;
            int? commandHr = null;
            if (action != "status") (ok, commandHr) = Set(source, action);
            var state = Read(source);
            JsonOutput.Write(new { type = "transport", backend = "directshow", action,
                supported = action == "status" ? state.Supported : ok,
                command_accepted = action == "status" ? (bool?)null : ok,
                hresult = commandHr ?? state.HResult, mode = state.Mode, state = Name(state.Mode),
                physical_end_verified = false });
            return (action == "status" ? state.Supported : ok) ? 0 : 4;
        }
        finally { Ds.Release(source); }
    }
}

internal sealed record CaptureOptions(string Device, string Output, int MaxSeconds,
    bool StopOnStdin, bool PlayAfterStream, bool StopOnFinish);

internal static class Capture
{
    private static void Connect(IFilterGraph graph, IPin output, IPin input, ref AmMediaType type)
    {
        IntPtr pointer = Marshal.AllocCoTaskMem(Marshal.SizeOf<AmMediaType>());
        try
        {
            Marshal.StructureToPtr(type, pointer, false);
            Ds.Check(graph.ConnectDirect(output, input, pointer), "ConnectDirect raw dvsd");
        }
        finally { Marshal.FreeCoTaskMem(pointer); }
    }

    public static int Run(CaptureOptions options)
    {
        object? graphObject = null, grabberObject = null, nullObject = null;
        IBaseFilter? source = null;
        RawPin? raw = null;
        IPin? grabberInput = null, grabberOutput = null, nullInput = null;
        RawSampleWriter? writer = null;
        IMediaFilter? control = null;
        ISampleGrabber? grabber = null;
        var stop = new StopRequest();
        var clock = new Stopwatch();
        bool deviceLost = false, graphStarted = false, tapePlayAttempted = false;
        int? graphError = null;
        Exception? fatal = null;
        string stopReason = "setup_error";
        (bool Supported, int? Mode, int? HResult) tapeState = (false, null, null);
        bool? cameraStopCommand = null;
        ConsoleCancelEventHandler cancel = (_, e) => { e.Cancel = true; stop.Request("user_stop"); };
        Console.CancelKeyPress += cancel;
        try
        {
            source = Devices.Open(options.Device);
            raw = Devices.FindRawCapturePin(source) ?? throw new InvalidOperationException("No raw dvsd capture pin was exposed by the native driver.");
            graphObject = Ds.Create(Ds.FilterGraph);
            var graph = (IFilterGraph)graphObject;
            control = (IMediaFilter)graphObject;
            var events = (IMediaEvent)graphObject;
            try { grabberObject = Ds.Create(Ds.SampleGrabber); }
            catch (COMException ex) when (ex.HResult == unchecked((int)0x80040154))
            { throw new InvalidOperationException("Windows Sample Grabber is unavailable; this native capture path is unsupported on this installation.", ex); }
            nullObject = Ds.Create(Ds.NullRenderer);
            var grabberFilter = (IBaseFilter)grabberObject;
            var nullFilter = (IBaseFilter)nullObject;
            grabber = (ISampleGrabber)grabberObject;
            Ds.Check(graph.AddFilter(source, "Panasonic raw DV"), "Add capture filter");
            Ds.Check(graph.AddFilter(grabberFilter, "Raw sample copy"), "Add sample filter");
            Ds.Check(graph.AddFilter(nullFilter, "Discard after saving"), "Add null renderer");
            Ds.Check(grabber.SetOneShot(false), "Disable one-shot");
            Ds.Check(grabber.SetBufferSamples(false), "Disable internal buffering");
            var preference = new AmMediaType { MajorType = raw.MediaType.MajorType, Subtype = Ds.Dvsd };
            Ds.Check(grabber.SetMediaType(ref preference), "Require raw dvsd");
            grabberInput = Devices.GetPin(grabberFilter, 0);
            grabberOutput = Devices.GetPin(grabberFilter, 1);
            nullInput = Devices.GetPin(nullFilter, 0);
            Connect(graph, raw.Pin, grabberInput, ref raw.MediaType);
            Ds.Check(graph.ConnectDirect(grabberOutput, nullInput, IntPtr.Zero), "ConnectDirect null renderer");
            Ds.Check(grabber.GetConnectedMediaType(out AmMediaType negotiated), "Read connected format");
            try
            {
                if (!Ds.IsDv(negotiated)) throw new InvalidDataException("The graph negotiated a non-DV format; capture was refused.");
            }
            finally { Ds.FreeType(ref negotiated); }
            Ds.Check(grabberOutput.ConnectionMediaType(out AmMediaType downstream), "Read downstream format");
            try
            {
                if (!Ds.IsDv(downstream) || downstream.MajorType != raw.MediaType.MajorType)
                    throw new InvalidDataException("The downstream graph changed the raw DV media type; capture was refused.");
            }
            finally { Ds.FreeType(ref downstream); }
            // Do not schedule copies against media timestamps or use them as file offsets.
            Ds.Check(control.SetSyncSource(IntPtr.Zero), "Disable renderer scheduling");
            string fullPath = Path.GetFullPath(options.Output);
            if (!string.Equals(Path.GetExtension(fullPath), ".dv", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("The raw output filename must end in .dv.");
            Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
            writer = new RawSampleWriter(fullPath);
            Ds.Check(grabber.SetCallback(writer, 1), "Set raw buffer callback");
            if (options.StopOnStdin)
            {
                _ = Task.Run(async () =>
                {
                    try
                    {
                        while (true)
                        {
                            string? line = await Console.In.ReadLineAsync();
                            if (line is null) { stop.Request("stdin_eof"); break; }
                            if (line.Trim().Equals("stop", StringComparison.OrdinalIgnoreCase))
                            { stop.Request("user_stop"); break; }
                        }
                    }
                    catch { stop.Request("stdin_closed"); }
                });
            }
            Ds.Check(control.Run(0), "Run raw capture graph");
            graphStarted = true;
            clock.Start();
            JsonOutput.Write(new { type = "started", backend = "directshow", output = fullPath,
                device = options.Device, subtype = "dvsd", max_sec = options.MaxSeconds, drop = (long?)null });
            double nextStatus = 0;
            while (stop.Reason is null)
            {
                if (writer.Failure is not null) { stopReason = writer.WriteError ? "write_error" : "buffer_error"; break; }
                if (clock.Elapsed.TotalSeconds >= options.MaxSeconds) { stopReason = "max_duration"; break; }
                // Some native drivers deliver no paused samples. In either case
                // install the sink and start the graph before requesting playback.
                if (options.PlayAfterStream && !tapePlayAttempted && (writer.Accepted > 0 || clock.Elapsed.TotalSeconds >= 1))
                {
                    tapePlayAttempted = true;
                    var play = Tape.Set(source, "play");
                    JsonOutput.Write(new { type = "transport", backend = "directshow", action = "play",
                        supported = play.Supported, command_accepted = play.Supported, hresult = play.HResult,
                        stream_warmed = writer.Accepted > 0, physical_end_verified = false });
                }
                while (events.GetEvent(out int eventCode, out IntPtr p1, out IntPtr p2, 0) == 0)
                {
                    try
                    {
                        if (eventCode == Ds.EcDeviceLost && p2 == IntPtr.Zero)
                        { deviceLost = true; stop.Request("device_lost"); }
                        else if (eventCode == Ds.EcErrorAbort)
                        { graphError = unchecked((int)p1.ToInt64()); stop.Request("graph_error"); }
                        else if (eventCode == Ds.EcComplete) stop.Request("stream_ended_unconfirmed");
                    }
                    finally { events.FreeEventParams(eventCode, p1, p2); }
                }
                if (clock.Elapsed.TotalSeconds >= nextStatus)
                {
                    tapeState = Tape.Read(source);
                    JsonOutput.Write(new { type = "status", backend = "directshow", bytes = writer.Received,
                        written = writer.Written, elapsed = clock.Elapsed.TotalSeconds, drop = (long?)null,
                        ovf = writer.Overflows, samples = writer.Samples, camera_mode = tapeState.Mode,
                        camera_state = Tape.Name(tapeState.Mode), transport_supported = tapeState.Supported,
                        physical_end_verified = false });
                    nextStatus = clock.Elapsed.TotalSeconds + 1;
                }
                Thread.Sleep(50);
            }
            if (stop.Reason is not null) stopReason = stop.Reason;
        }
        catch (Exception ex) { fatal = ex; stopReason = graphStarted ? "capture_error" : "setup_error"; }
        finally
        {
            // Stop native delivery first, then drain every accepted queued buffer.
            if (control is not null && graphStarted)
            {
                try { int hr = control.Stop(); if (hr < 0) graphError ??= hr; }
                catch (Exception ex) { fatal ??= ex; }
            }
            if (grabber is not null)
            {
                try { grabber.SetCallback(null, 1); } catch { }
            }
            if (source is not null && options.StopOnFinish && graphStarted)
            {
                try { cameraStopCommand = Tape.Set(source, "stop").Supported; }
                catch { cameraStopCommand = false; }
            }
            if (source is not null)
            {
                try { tapeState = Tape.Read(source); } catch { }
            }
            writer?.Complete();
            clock.Stop();
            Ds.Release(nullInput); Ds.Release(grabberOutput); Ds.Release(grabberInput);
            raw?.Dispose();
            // Release each owning RCW once; interface casts are aliases, not owners.
            Ds.Release(graphObject); Ds.Release(nullObject); Ds.Release(grabberObject); Ds.Release(source);
            Console.CancelKeyPress -= cancel;
            GC.KeepAlive(writer);
        }
        long fileBytes = writer is not null && File.Exists(writer.OutputPath) ? new FileInfo(writer.OutputPath).Length : 0;
        bool writeError = writer?.WriteError == true || (writer is null && fatal is IOException or UnauthorizedAccessException);
        bool failed = fatal is not null || writer?.Failure is not null || graphError is not null || deviceLost;
        if (writeError) stopReason = "write_error";
        else if (writer?.Failure is not null) stopReason = "buffer_error";
        JsonOutput.Write(new { type = "done", backend = "directshow", bytes = writer?.Received ?? 0,
            written = fileBytes, accepted = writer?.Accepted ?? 0, elapsed = clock.Elapsed.TotalSeconds,
            physical_end_verified = false, stop_reason = stopReason, device_lost = deviceLost,
            drop = (long?)null, ovf = writer?.Overflows ?? 0, write_error = writeError ? 1 : 0,
            partial = failed || fileBytes == 0, success = !failed && fileBytes > 0,
            camera_stopped = tapeState.Mode.HasValue ? tapeState.Mode == Ds.ModeStop : (bool?)null,
            camera_stop_command_accepted = cameraStopCommand, end = "unconfirmed", hresult = graphError,
            message = fatal?.Message ?? writer?.Failure?.Message,
            output = writer?.OutputPath ?? Path.GetFullPath(options.Output) });
        return failed || fileBytes == 0 ? 2 : 0;
    }
}
