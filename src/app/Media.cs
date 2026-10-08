// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace DvTape;

public record AudioMode(int Rate, int Bits, int Pairs);
public record AudioRun(long Start, long End, AudioMode? Mode);
public record RawAudit(string Source, long Bytes, string Sha256, int FrameBytes, double Fps,
    long Frames, int TrailingBytes, long StructureErrors, long SourceFlaggedFrames,
    long? PlaceholderTailStart, List<AudioRun> AudioRuns);
public record CaptureOutcome(bool Saved, bool BytesAgree, bool PhysicalEndVerified, bool ZeroLossVerified);

public static class Media
{
    public static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    public static double ElapsedSeconds(JsonElement status)
    {
        return status.TryGetProperty("elapsed", out var node) && node.TryGetDouble(out var seconds) && double.IsFinite(seconds) ? Math.Clamp(seconds, 0, 86400) : 0;
    }
    public static CaptureOutcome AssessCapture(JsonElement? done, int exitCode, long actualBytes)
    {
        bool Number(string key, out long value)
        { value = 0; return done.HasValue && done.Value.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value); }
        bool Flag(string key) => done.HasValue && done.Value.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.True;
        bool bytesAgree = actualBytes > 0 && Number("written", out long written) && written == actualBytes && Number("bytes", out long bytes) && bytes == actualBytes;
        bool fault = Flag("device_lost") || Flag("transport_failed") || new[] { "drop", "ovf", "write_error" }.Any(k => Number(k, out var value) && value != 0);
        bool zeroLoss = new[] { "drop", "ovf", "write_error" }.All(k => Number(k, out var value) && value == 0) && !fault;
        return new(exitCode == 0 && bytesAgree && !fault, bytesAgree, Flag("physical_end_verified"), zeroLoss);
    }
    public static string SafeName(string value)
    {
        value = value.Trim().TrimEnd('.', ' ');
        if (value.Length == 0 || value.IndexOfAny("<>:\"/\\|?*".ToCharArray()) >= 0 || value.Any(char.IsControl))
            throw new IOException("磁带名称不能为空，也不能包含 <> : \" / \\ | ? *。");
        var first = value.Split('.')[0].ToUpperInvariant();
        if (new[] { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" }.Contains(first))
            throw new IOException("请换一个磁带名称，这个名称被 Windows 保留。");
        if (value.Length > 100) throw new IOException("磁带名称请控制在 100 字以内。");
        return value;
    }
    public static ProcessStartInfo Command(string exe, IEnumerable<string> args)
    {
        var p = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 };
        foreach (var arg in args) p.ArgumentList.Add(arg);
        return p;
    }
    public static void WriteNewJson(string path, object data)
    {
        using var f = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read);
        JsonSerializer.Serialize(f, data, JsonOptions);
        f.Flush(true);
    }
    public static AudioMode? DecoderAudio(ReadOnlySpan<byte> frame)
    {
        for (int s = 0; s < 10; s++)
        {
            var off = s * 12000 + 480 + ((s & 1) == 1 ? 0 : 3840) + 3;
            if (off + 5 > frame.Length || frame[off] != 0x50) continue;
            int frequency = (frame[off + 4] >> 3) & 7, quant = frame[off + 4] & 7, type = frame[off + 3] & 31;
            if (frequency > 2 || quant > 1 || (type != 0 && type != 2 && type != 3)) return null;
            int pairs = type == 0 ? 1 : type == 2 ? 2 : 4;
            if (pairs == 1 && quant == 1 && frequency == 2) pairs = 2;
            return new(new[] { 48000, 44100, 32000 }[frequency], quant == 0 ? 16 : 12, pairs);
        }
        return null;
    }
    public static RawAudit Audit(string path, CancellationToken token, Action<string>? progress = null)
    {
        var initial = new FileInfo(path); long bytes = initial.Length; var modified = initial.LastWriteTimeUtc;
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        var header = new byte[4]; if (f.Read(header) != 4) throw new IOException("原始文件没有完整的 DV 帧。");
        bool pal = (header[3] & 128) != 0; int sequences = pal ? 12 : 10, size = sequences * 12000;
        double fps = pal ? 25 : 30000.0 / 1001;
        f.Position = 0; long count = bytes / size, errors = 0, flagged = 0;
        var frame = new byte[size]; var runs = new List<AudioRun>();
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[]? tailAudio = null; long? tailStart = null; var watch = Stopwatch.StartNew();
        for (long n = 0; n < count; n++)
        {
            token.ThrowIfCancellationRequested(); f.ReadExactly(frame); hash.AppendData(frame);
            bool bad = false, placeholder = true, structureOk = true, hasAudioSource = false;
            using var audioHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            for (int s = 0; s < sequences; s++)
                for (int k = 0; k < 150; k++)
                {
                    int off = s * 12000 + k * 80;
                    int section = k == 0 ? 0 : k <= 2 ? 1 : k <= 5 ? 2 : (k - 6) % 16 == 0 ? 3 : 4;
                    int number = section == 0 ? 0 : section == 1 ? k - 1 : section == 2 ? k - 3 : section == 3 ? (k - 6) / 16 : k - 7 - (k - 6) / 16;
                    if ((frame[off] >> 5) != section || (frame[off + 1] >> 4) != s || frame[off + 2] != number || (section == 0 && ((frame[off + 3] & 128) != 0) != pal))
                    { errors++; structureOk = false; placeholder = false; continue; }
                    if (section == 4)
                    {
                        if ((frame[off + 3] >> 4) != 0) bad = true;
                        for (int j = off + 3; j < off + 80; j++) if (frame[j] != 255) { placeholder = false; break; }
                    }
                    else if (section == 3)
                    {
                        if (frame[off + 3] == 0x50) hasAudioSource = true;
                        audioHash.AppendData(frame, off + 8, 72);
                    }
                }
            if (bad) flagged++;
            var mode = structureOk ? DecoderAudio(frame) : null;
            if (mode?.Pairs > 2) throw new IOException("检测到超过两组立体声音轨；已保留原始 DV，当前版本拒绝丢弃额外声音。");
            if (runs.Count > 0 && runs[^1].Mode == mode) runs[^1] = runs[^1] with { End = n + 1 };
            else runs.Add(new(n, n + 1, mode));
            var ah = audioHash.GetHashAndReset();
            // A static scene with real AAUX/audio must never be treated as empty tape.
            if (structureOk && placeholder && !hasAudioSource)
            {
                if (tailAudio == null || !tailAudio.AsSpan().SequenceEqual(ah)) { tailStart = n; tailAudio = ah; }
            }
            else { tailStart = null; tailAudio = null; }
            if (watch.ElapsedMilliseconds > 1000) { progress?.Invoke($"检查原始录像：{100.0 * n / Math.Max(1, count):F0}%"); watch.Restart(); }
        }
        int trailing = (int)(bytes % size); if (trailing > 0) { var b = new byte[trailing]; f.ReadExactly(b); hash.AppendData(b); }
        initial.Refresh(); if (initial.Length != bytes || initial.LastWriteTimeUtc != modified) throw new IOException("原始文件仍在变化，请等待采集结束。");
        if (count == 0) throw new IOException("原始文件没有完整 DV 帧；文件已保留。");
        // Require >= 5 seconds of structural all-FF video + identical audio payload.
        if (tailStart == null || count - tailStart < Math.Ceiling(fps * 5) || tailStart == 0) tailStart = null;
        return new(Path.GetFullPath(path), bytes, Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant(), size, fps, count, trailing, errors, flagged, tailStart, runs);
    }
    public static List<string> ExportArguments(string source, string output, RawAudit audit, bool trimPlaceholder, string graph)
    {
        if (File.Exists(output)) throw new IOException("播放文件已存在，请换名称；不会覆盖原文件。");
        if (audit.StructureErrors != 0) throw new IOException("原始 DV 帧结构不完整，已保留原文件。请检查连接后重采；当前导出不会隐藏这个问题。");
        var end = trimPlaceholder && audit.PlaceholderTailStart.HasValue ? audit.PlaceholderTailStart.Value : audit.Frames;
        var runs = audit.AudioRuns.Where(x => x.Start < end).Select(x => x with { End = Math.Min(x.End, end) }).ToList();
        if (runs.Count > 128) throw new IOException("声音格式切换超过 128 次，原始文件保留；需要分段导出。");
        var args = new List<string> { "-hide_banner", "-nostdin", "-loglevel", "warning", "-nostats", "-progress", "pipe:1", "-n" };
        var filters = new List<string>(); var concat = new StringBuilder();
        for (int i = 0; i < runs.Count; i++)
        {
            var run = runs[i]; long frames = run.End - run.Start;
            args.AddRange(new[] { "-threads", "1", "-f", "dv", "-i", $"subfile,,start,{run.Start * audit.FrameBytes},end,{run.End * audit.FrameBytes},,:{Path.GetFullPath(source)}" });
            long samples = (long)Math.Round(frames * 48000 / audit.Fps);
            filters.Add($"[{i}:v:0]bwdif=mode=send_field:parity=bff:deint=all,trim=end_frame={frames * 2},setpts=PTS-STARTPTS,format=yuv420p[v{i}]");
            concat.Append($"[v{i}]");
            for (int pair = 0; pair < 2; pair++)
            {
                var audio = pair < (run.Mode?.Pairs ?? 0) ? $"[{i}:a:{pair}]aresample=48000:async=1:first_pts=0,aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo,apad," : "anullsrc=channel_layout=stereo:sample_rate=48000,";
                filters.Add(audio + $"atrim=end_sample={samples},asetpts=N/SR/TB[a{i}_{pair}]"); concat.Append($"[a{i}_{pair}]");
            }
        }
        filters.Add(concat + $"concat=n={runs.Count}:v=1:a=2[v][a0][a1]");
        using (var f = new FileStream(graph, FileMode.CreateNew)) using (var w = new StreamWriter(f, new UTF8Encoding(false))) w.Write(string.Join(";\n", filters));
        // FFmpeg 7+ loads option values from files with -/option. The old
        // -filter_complex_script was removed in FFmpeg 9. Keep the graph in
        // a UTF-8 file so large audio plans do not inflate the Windows command.
        args.AddRange(new[] { "-filter_complex_threads", "1", "-/filter_complex", graph, "-map", "[v]", "-map", "[a0]", "-map", "[a1]", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-threads", "4", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-b:a", "192k", "-movflags", "+faststart", "-metadata:s:a:0", "title=DV audio pair 1", "-metadata:s:a:1", "title=DV audio pair 2 (silence if absent)", "-disposition:a:0", "default", "-disposition:a:1", "0", output });
        return args;
    }
    public static async Task<int> Run(string exe, IEnumerable<string> args, string logPath, CancellationToken token, Action<string>? progress = null)
    {
        using var process = new Process { StartInfo = Command(exe, args) };
        using var log = new StreamWriter(new FileStream(logPath, FileMode.CreateNew), new UTF8Encoding(false));
        process.Start(); process.StandardInput.Close();
        using var registration = token.Register(() => { try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { } });
        var errors = Task.Run(async () =>
        {
            long bytes = 0, lines = 0; Exception? failure = null;
            while (await process.StandardError.ReadLineAsync() is { } line)
            {
                lines++; bytes += Encoding.UTF8.GetByteCount(line) + 1;
                if (bytes <= 1 << 20 && failure == null)
                    try { await log.WriteLineAsync(line); }
                    catch (IOException ex) { failure = ex; try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { } }
            }
            if (failure != null) throw failure;
            if (bytes > 1 << 20) await log.WriteLineAsync($"Diagnostic log capped at 1 MiB; total {lines} messages / {bytes} bytes. Source DV retained.");
        });
        try
        {
            while (await process.StandardOutput.ReadLineAsync() is { } line) if (line.StartsWith("out_time=") && !line.StartsWith("out_time=-")) progress?.Invoke(line[9..]);
            await process.WaitForExitAsync(); await errors; await log.FlushAsync();
            token.ThrowIfCancellationRequested(); return process.ExitCode;
        }
        finally
        {
            try { if (!process.HasExited) { process.Kill(true); await process.WaitForExitAsync(); } } catch (InvalidOperationException) { }
            // Keep stderr draining until the child has exited before log disposal.
            await errors;
        }
    }
    public static async Task Export(string ffmpeg, string ffprobe, string raw, string mp4, bool trim, string records, CancellationToken token, Action<string> progress)
    {
        var audit = await Task.Run(() => Audit(raw, token, progress), token);
        WriteNewJson(Path.Combine(records, "原始文件检查.json"), audit);
        var args = ExportArguments(raw, mp4, audit, trim, Path.Combine(records, "转换设置.txt"));
        var original = new FileInfo(raw); long size = original.Length; var modified = original.LastWriteTimeUtc;
        var rc = await Run(ffmpeg, args, Path.Combine(records, "转换日志.txt"), token, x => progress("正在生成播放文件：" + x));
        if (rc != 0) throw new IOException("播放文件转换中断，原始 DV 已保留。详见转换日志。");
        progress("正在检查整段播放文件…");
        var verifyPath = Path.Combine(records, "播放检查日志.txt");
        var verify = await Run(ffmpeg, new[] { "-hide_banner", "-nostdin", "-v", "error", "-progress", "pipe:1", "-i", mp4, "-map", "0:v:0", "-map", "0:a", "-f", "null", "-" }, verifyPath, token, x => progress("正在检查播放文件：" + x));
        if (verify != 0 || new FileInfo(verifyPath).Length != 0) throw new IOException("播放文件检查发现问题。原始 DV 已保留，请查看记录。");
        using var probe = new Process { StartInfo = Command(ffprobe, new[] { "-v", "error", "-show_streams", "-show_format", "-of", "json", mp4 }) };
        probe.Start(); var stdout = probe.StandardOutput.ReadToEndAsync(); var stderr = probe.StandardError.ReadToEndAsync();
        using var probeCancellation = token.Register(() => { try { if (!probe.HasExited) probe.Kill(true); } catch (InvalidOperationException) { } });
        await probe.WaitForExitAsync(); string probeOutput = await stdout, probeError = await stderr; token.ThrowIfCancellationRequested();
        if (probe.ExitCode != 0 || probeError.Length > 0) throw new IOException("无法读取播放文件信息。");
        using var doc = JsonDocument.Parse(probeOutput); var streams = doc.RootElement.GetProperty("streams").EnumerateArray().ToList();
        var video = streams.Single(x => x.GetProperty("codec_type").GetString() == "video");
        var audio = streams.Where(x => x.GetProperty("codec_type").GetString() == "audio").ToList();
        long kept = trim && audit.PlaceholderTailStart.HasValue ? audit.PlaceholderTailStart.Value : audit.Frames;
        double expectedSeconds = kept / audit.Fps;
        if (video.GetProperty("codec_name").GetString() != "h264" || video.GetProperty("width").GetInt32() != 720 || video.GetProperty("height").GetInt32() != (audit.FrameBytes == 144000 ? 576 : 480) || long.Parse(video.GetProperty("nb_frames").GetString()!, CultureInfo.InvariantCulture) != kept * 2 || audio.Count != 2 || Math.Abs(double.Parse(video.GetProperty("duration").GetString()!, CultureInfo.InvariantCulture) - expectedSeconds) > 1 / audit.Fps || audio.Any(x => x.GetProperty("codec_name").GetString() != "aac" || x.GetProperty("sample_rate").GetString() != "48000" || x.GetProperty("channels").GetInt32() != 2 || Math.Abs(double.Parse(x.GetProperty("duration").GetString()!, CultureInfo.InvariantCulture) - expectedSeconds) > 0.05))
            throw new IOException("播放文件帧数或声音时长不符，原始 DV 已保留。");
        original.Refresh(); if (original.Length != size || original.LastWriteTimeUtc != modified) throw new IOException("原始文件在导出中变化，不能报告验证完成。");
        WriteNewJson(Path.Combine(records, "播放文件检查.json"), new { status = "verified_viewing_copy", duration_seconds = expectedSeconds, source_sha256 = audit.Sha256, source_unchanged = true, raw_trailing_bytes_retained = audit.TrailingBytes, source_flagged_frames = audit.SourceFlaggedFrames, frames = kept * 2, audio_tracks = 2, removed_placeholder_frames = audit.Frames - kept, unknown_audio_ranges = audit.AudioRuns.Where(x => x.Mode == null && x.Start < kept).Select(x => new { start_frame = x.Start, end_frame_exclusive = Math.Min(x.End, kept) }), audio_policy = "First AAUX source pack following FFmpeg interpretation. Unknown/invalid packs synthesize silence in MP4; all source bytes remain in raw DV. Absent second pair is silence.", physical_tape_completeness = "See capture record; MP4 validation does not prove tape end", source_quality = "Source read defects may remain or be concealed in MP4. Original DV retained", probe = doc.RootElement.Clone() });
    }
}
