// SPDX-License-Identifier: GPL-3.0-or-later
using DvTape;
using System.Security.Cryptography;
using System.Text.Json;

var temp = Path.Combine(Path.GetTempPath(), "DV 导出测试 " + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(temp);
int passed = 0;
void Check(bool value, string name) { if (!value) throw new Exception("FAIL: " + name); passed++; Console.WriteLine("PASS: " + name); }
bool Refuses(Action work) { try { work(); return false; } catch (IOException) { return true; } }
byte[] Frame(bool placeholder = false, bool audioSource = true, byte audioValue = 0x23)
{
    var frame = new byte[144000];
    for (int s = 0; s < 12; s++) for (int k = 0; k < 150; k++)
    {
        int off = s * 12000 + k * 80;
        int section = k == 0 ? 0 : k <= 2 ? 1 : k <= 5 ? 2 : (k - 6) % 16 == 0 ? 3 : 4;
        int number = section == 0 ? 0 : section == 1 ? k - 1 : section == 2 ? k - 3 : section == 3 ? (k - 6) / 16 : k - 7 - (k - 6) / 16;
        frame[off] = (byte)(section << 5); frame[off + 1] = (byte)(s << 4); frame[off + 2] = (byte)number;
        if (section == 0) frame[off + 3] = 128;
        if (section == 4) Array.Fill(frame, placeholder ? (byte)255 : (byte)1, off + 3, 77);
        if (section == 3)
        {
            Array.Fill(frame, audioValue, off + 8, 72);
            frame[off + 3] = audioSource ? (byte)0x50 : (byte)0xff;
            frame[off + 4] = 0; frame[off + 5] = 0; frame[off + 6] = 0; frame[off + 7] = 0;
        }
    }
    return frame;
}
void WriteFrames(string p, IEnumerable<byte[]> frames, int trailing = 0)
{ using var f = new FileStream(p, FileMode.CreateNew); foreach (var frame in frames) f.Write(frame); f.Write(new byte[trailing]); }
try
{
    Check(Media.SafeName("家庭录像") == "家庭录像", "Chinese tape name retained");
    Check(Refuses(() => Media.SafeName("CON.txt")) && Refuses(() => Media.SafeName("../recording")), "Windows reserved names and traversal refused");
    var p = Media.Command("ffmpeg", new[] { "some path/家庭录像 $(literal).dv" });
    Check(!p.UseShellExecute && p.ArgumentList[0] == "some path/家庭录像 $(literal).dv", "Arguments remain literal without shell execution");
    using var progressEvent = JsonDocument.Parse("{\"type\":\"status\",\"elapsed\":1.235,\"bytes\":144000}");
    Check(Media.ElapsedSeconds(progressEvent.RootElement) == 1.235, "DirectShow fractional elapsed seconds are supported");
    using var stopped = JsonDocument.Parse("{\"bytes\":144000,\"written\":144000,\"physical_end_verified\":false,\"state\":64,\"drop\":null,\"ovf\":null,\"write_error\":0}");
    var partial = Media.AssessCapture(stopped.RootElement, 0, 144000);
    Check(partial.Saved && !partial.PhysicalEndVerified && !partial.ZeroLossVerified, "Ordinary Stop64 and unknown drop counters cannot prove tape end or zero loss");
    using var disconnected = JsonDocument.Parse("{\"bytes\":144000,\"written\":144000,\"device_lost\":true,\"drop\":0,\"ovf\":0,\"write_error\":0}");
    Check(!Media.AssessCapture(disconnected.RootElement, 0, 144000).Saved && !Media.AssessCapture(null, 0, 144000).Saved, "Disconnect and missing final event cannot be marked successful");
    string normal = Path.Combine(temp, "普通静止有声音.dv");
    WriteFrames(normal, Enumerable.Range(0, 140).Select(_ => Frame(true, true)), 12345);
    var a = Media.Audit(normal, CancellationToken.None);
    Check(a.Frames == 140 && a.TrailingBytes == 12345 && a.StructureErrors == 0, "Complete frame count and partial trailing bytes retained");
    Check(a.PlaceholderTailStart == null, "Static/error video with recorded audio metadata is never trimmed");
    Check(a.Sha256 == Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(normal))).ToLowerInvariant(), "Audit SHA covers all bytes including partial tail");
    string tail = Path.Combine(temp, "占位.dv");
    WriteFrames(tail, Enumerable.Range(0, 5).Select(_ => Frame()).Concat(Enumerable.Range(0, 126).Select(_ => Frame(true, false))));
    var b = Media.Audit(tail, CancellationToken.None);
    Check(b.PlaceholderTailStart == 5 && b.Frames == 131, "Only entire terminal placeholder run is identified");
    var argsFull = Media.ExportArguments(tail, Path.Combine(temp, "all.mp4"), b, false, Path.Combine(temp, "all.graph"));
    Check(argsFull.Any(x => x.Contains("end,18864000,")), "Default export retains all complete frames");
    var argsTrim = Media.ExportArguments(tail, Path.Combine(temp, "trim.mp4"), b, true, Path.Combine(temp, "trim.graph"));
    Check(argsTrim.Any(x => x.Contains("end,720000,")) && new FileInfo(tail).Length == b.Bytes, "Opt-in placeholder trim changes derivative only");
    string changing = Path.Combine(temp, "最后声音改变.dv");
    WriteFrames(changing, Enumerable.Range(0, 5).Select(_ => Frame()).Concat(Enumerable.Range(0, 125).Select(_ => Frame(true, false))).Append(Frame(true, false, 0x49)));
    Check(Media.Audit(changing, CancellationToken.None).PlaceholderTailStart == null, "Changed audio at last frame prevents tail deletion");
    var twelve = Frame(); int pack = 4323; twelve[pack + 4] = 17;
    Check(Media.DecoderAudio(twelve) == new AudioMode(32000, 12, 2), "32 kHz nonlinear four-channel audio decoded as two stereo pairs");
    var mixed = b with { PlaceholderTailStart = null, AudioRuns = new() { new(0, 50, new(48000, 16, 1)), new(50, 100, new(32000, 12, 2)), new(100, 131, new(48000, 16, 1)) } };
    var mixedArgs = Media.ExportArguments(tail, Path.Combine(temp, "mixed.mp4"), mixed, false, Path.Combine(temp, "mixed.graph"));
    var graphText = File.ReadAllText(Path.Combine(temp, "mixed.graph"));
    Check(mixedArgs.Count(x => x == "-i") == 3 && graphText.Contains("[1:a:1]") && graphText.Contains("concat=n=3:v=1:a=2"), "Audio format transitions preserve both pairs in one conversion");
    string exists = Path.Combine(temp, "已有.mp4"); File.WriteAllText(exists, "keep");
    Check(Refuses(() => Media.ExportArguments(normal, exists, a, false, Path.Combine(temp, "no.graph"))) && File.ReadAllText(exists) == "keep", "Existing output refused without changing bytes");
    var damaged = b with { StructureErrors = 1 };
    Check(Refuses(() => Media.ExportArguments(tail, Path.Combine(temp, "bad.mp4"), damaged, false, Path.Combine(temp, "bad.graph"))), "Misaligned DIF structure refuses conversion");
    using var cancel = new CancellationTokenSource(); cancel.Cancel(); bool canceled = false;
    try { Media.Audit(tail, cancel.Token); } catch (OperationCanceledException) { canceled = true; }
    Check(canceled && new FileInfo(tail).Length == b.Bytes, "Cancellation leaves original DV untouched");
    if (args.Length == 3)
    {
        string ffmpeg = args[0], ffprobe = args[1], fixture = args[2];
        string exportDir = Path.Combine(temp, "实际转换 记录"); Directory.CreateDirectory(exportDir);
        string output = Path.Combine(temp, "真实短 样片.mp4");
        string testSource = Path.Combine(temp, "家庭录像 有空格.dv"); File.Copy(fixture, testSource);
        var originalHash = SHA256.HashData(File.ReadAllBytes(testSource));
        await Media.Export(ffmpeg, ffprobe, testSource, output, false, exportDir, CancellationToken.None, Console.WriteLine);
        using var verified = JsonDocument.Parse(File.ReadAllText(Path.Combine(exportDir, "播放文件检查.json")));
        Check(File.Exists(output) && verified.RootElement.GetProperty("frames").GetInt64() == 40 && verified.RootElement.GetProperty("audio_tracks").GetInt32() == 2, "Real 20-frame DV export with Chinese and space paths validates 40 video frames and both audio tracks");
        Check(originalHash.AsSpan().SequenceEqual(SHA256.HashData(File.ReadAllBytes(testSource))), "Real export leaves every source DV byte unchanged");
        var slowArgs = new[] { "-hide_banner", "-nostdin", "-v", "error", "-stats_period", "0.1", "-progress", "pipe:1", "-re", "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=25", "-t", "30", "-f", "null", "-" };
        using var processCancel = new CancellationTokenSource(500); var watch = System.Diagnostics.Stopwatch.StartNew(); bool stoppedChild = false;
        try { await Media.Run(ffmpeg, slowArgs, Path.Combine(temp, "cancel-child.log"), processCancel.Token); } catch (OperationCanceledException) { stoppedChild = true; }
        Check(stoppedChild && watch.Elapsed.TotalSeconds < 5, "Cancel terminates and awaits the conversion child promptly");
        watch.Restart(); bool callbackFailed = false;
        try { await Media.Run(ffmpeg, slowArgs, Path.Combine(temp, "callback-child.log"), CancellationToken.None, _ => throw new InvalidOperationException("test callback failure")); }
        catch (InvalidOperationException e) when (e.Message == "test callback failure") { callbackFailed = true; }
        Check(callbackFailed && watch.Elapsed.TotalSeconds < 5, "Progress callback exception still terminates and awaits child");
        string boundedLog = Path.Combine(temp, "bounded-source-errors.log");
        await Media.Run(ffmpeg, new[] { "-hide_banner", "-nostdin", "-v", "repeat+warning", "-f", "dv", "-i", normal, "-map", "0:v:0", "-f", "null", "-" }, boundedLog, CancellationToken.None);
        Check(new FileInfo(boundedLog).Length <= (1 << 20) + 2000, "Damaged source diagnostics cannot grow without limit");
    }
    Console.WriteLine($"{passed} checks passed; no camera used.");
}
finally { Directory.Delete(temp, true); }
