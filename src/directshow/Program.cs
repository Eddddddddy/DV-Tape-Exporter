// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Encodings.Web;
using System.Text;

namespace DvDirectShow;

internal static class JsonOutput
{
    private static readonly object Gate = new();
    private static readonly JsonSerializerOptions Options = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    public static void Write(object value)
    {
        lock (Gate) Console.WriteLine(JsonSerializer.Serialize(value, Options));
    }
}

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        try
        {
            if (args.Length == 0 || args.Contains("--help"))
            {
                Console.WriteLine("dv-directshow --list-json\ncapture --device MONIKER --output FILE.dv [--stop-on-stdin] [--max-sec 10800] [--play-after-stream] [--stop-on-finish]\n--device MONIKER --tape status|play|rew|stop\n--self-test --self-test-dir DIRECTORY");
                return 0;
            }
            var flags = new HashSet<string>();
            var values = new Dictionary<string, string>();
            bool capture = args[0] == "capture";
            var valueOptions = new HashSet<string> { "--device", "--output", "--max-sec", "--tape", "--self-test-dir" };
            var flagOptions = new HashSet<string> { "--list-json", "--stop-on-stdin", "--play-after-stream", "--stop-on-finish", "--self-test" };
            for (int i = capture ? 1 : 0; i < args.Length; i++)
            {
                string arg = args[i];
                if (valueOptions.Contains(arg))
                {
                    if (++i == args.Length || args[i].StartsWith("--", StringComparison.Ordinal))
                        throw new ArgumentException($"{arg} requires a value.");
                    if (!values.TryAdd(arg, args[i])) throw new ArgumentException($"Duplicate option {arg}.");
                }
                else if (flagOptions.Contains(arg))
                {
                    if (!flags.Add(arg)) throw new ArgumentException($"Duplicate option {arg}.");
                }
                else throw new ArgumentException($"Unknown option {arg}.");
            }
            if (flags.Contains("--self-test")) return SelfTest.Run(Require(values, "--self-test-dir"));
            if (!OperatingSystem.IsWindows())
            {
                JsonOutput.Write(new { type = "error", backend = "directshow", reason = "windows_required",
                    message = "Native DirectShow enumeration and capture require Windows. No hardware was accessed." });
                return 3;
            }
            if (flags.Contains("--list-json"))
            {
                if (capture || values.Count != 0 || flags.Count != 1) throw new ArgumentException("--list-json must be used on its own.");
                return Devices.List();
            }
            if (values.TryGetValue("--tape", out string? action))
            {
                if (capture || values.ContainsKey("--output")) throw new ArgumentException("Use tape controls separately from capture.");
                if (action is not ("status" or "play" or "rew" or "stop")) throw new ArgumentException("Tape action must be status, play, rew or stop.");
                return Tape.Command(Require(values, "--device"), action);
            }
            if (!capture) throw new ArgumentException("Use capture, --list-json or --tape.");
            int seconds = 10800;
            if (values.TryGetValue("--max-sec", out string? max) && (!int.TryParse(max, out seconds) || seconds < 1 || seconds > 10800))
                throw new ArgumentException("--max-sec must be between 1 and 10800 (three hours).");
            return Capture.Run(new CaptureOptions(Require(values, "--device"), Require(values, "--output"), seconds,
                flags.Contains("--stop-on-stdin"), flags.Contains("--play-after-stream"), flags.Contains("--stop-on-finish")));
        }
        catch (Exception ex)
        {
            JsonOutput.Write(new { type = "error", backend = "directshow", reason = "command_failed", message = ex.Message,
                hresult = ex is COMException ? (int?)ex.HResult : null });
            return 2;
        }
    }
    private static string Require(Dictionary<string, string> options, string key) =>
        options.TryGetValue(key, out string? value) && !string.IsNullOrWhiteSpace(value) ? value : throw new ArgumentException($"{key} is required.");
}

internal static class SelfTest
{
    public static int Run(string directory)
    {
        Directory.CreateDirectory(directory);
        string path = Path.Combine(Path.GetFullPath(directory), "原始 DV " + Guid.NewGuid().ToString("N") + ".dv");
        var data = new byte[144000];
        Random.Shared.NextBytes(data);
        using var expected = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        IntPtr buffer = Marshal.AllocCoTaskMem(data.Length);
        const int count = 128;
        long bytes;
        try
        {
            Marshal.Copy(data, 0, buffer, data.Length);
            using var writer = new RawSampleWriter(path);
            for (int i = 0; i < count; i++) { expected.AppendData(data); writer.BufferCB(i / 25.0, buffer, data.Length); }
            writer.Complete();
            bytes = writer.Written;
            if (writer.Failure is not null || writer.Received != (long)count * data.Length || writer.Accepted != writer.Written || writer.Overflows != 0)
                throw new InvalidOperationException("Raw callback drain/counter test failed.");
        }
        finally { Marshal.FreeCoTaskMem(buffer); }
        byte[] wanted = expected.GetHashAndReset();
        using (var read = File.OpenRead(path))
            if (!SHA256.HashData(read).SequenceEqual(wanted)) throw new InvalidOperationException("Raw sample bytes changed.");
        bool refusedOverwrite = false;
        try { using var blocked = new RawSampleWriter(path); }
        catch (IOException) { refusedOverwrite = true; }
        if (!refusedOverwrite) throw new InvalidOperationException("CreateNew did not refuse an existing output.");
        using (var read = File.OpenRead(path))
            if (!SHA256.HashData(read).SequenceEqual(wanted)) throw new InvalidOperationException("The existing output changed.");
        if (Marshal.SizeOf<AmMediaType>() != 88 || Marshal.OffsetOf<AmMediaType>(nameof(AmMediaType.FormatPtr)).ToInt32() != 80)
            throw new InvalidOperationException("The x64 AM_MEDIA_TYPE layout is incorrect.");
        if (Ds.ModePlay != 4296 || Ds.ModeStop != 4297 || Ds.ModeRewind != 4301)
            throw new InvalidOperationException("Transport constants do not match the Windows SDK.");
        if (!Devices.MatchesTarget("@device:pnp:usb#vid_04da&pid_231a", "Camera", "") ||
            Devices.MatchesTarget("@device:pnp:usb#vid_04da&pid_2316", "USB Storage", "") ||
            Devices.MatchesTarget("@device:pnp:usb#vid_046d", "Webcam", ""))
            throw new InvalidOperationException("The device identity allow-list is incorrect.");
        File.Delete(path);
        JsonOutput.Write(new { type = "self_test", success = true, hardware_accessed = false,
            checks = new[] { "callback_bytes_unchanged", "bounded_queue_drained", "counters_match_file", "unicode_path", "atomic_no_overwrite", "x64_media_type_layout", "transport_constants", "device_identity_allow_list" },
            bytes });
        return 0;
    }
}
