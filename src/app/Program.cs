// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace DvTape;

static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }
}

record Camera(string Id, string Name, string Backend)
{
    public override string ToString() => Name;
}

sealed class MainForm : Form
{
    readonly TextBox tape = new() { Text = "磁带-" + DateTime.Now.ToString("yyyy-MM-dd"), PlaceholderText = "例如：旅行录像", Dock = DockStyle.Fill };
    readonly TextBox folder = new() { ReadOnly = true, BorderStyle = BorderStyle.None, BackColor = Color.White, Dock = DockStyle.Fill };
    readonly Button primary = new() { Text = "开始采集", Height = 48, Width = 210, FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(34, 91, 73), ForeColor = Color.White, Font = new Font("Microsoft YaHei UI", 12, FontStyle.Bold) };
    readonly Button more = new() { Text = "更多 ▾", AutoSize = true, FlatStyle = FlatStyle.Flat };
    readonly ContextMenuStrip menu = new();
    readonly ToolStripMenuItem chooseCamera = new("选择摄像机") { Visible = false };
    readonly ToolStripMenuItem confirmEnd = new("记录卷尾确认") { Enabled = false };
    readonly Label connection = new() { Text = "正在查找摄像机…", AutoSize = true, ForeColor = Color.FromArgb(100, 110, 105), MaximumSize = new(620, 0) };
    readonly Label status = new() { Text = "准备好磁带后，点击“开始采集”。", AutoSize = true, MaximumSize = new(620, 0) };
    readonly Label counters = new() { Text = "00:00:00   ·   0.00 GB", AutoSize = true, Font = new Font("Microsoft YaHei UI", 22), ForeColor = Color.FromArgb(34, 91, 73) };
    readonly LinkLabel resultLink = new() { Text = "打开已保存的录像", AutoSize = true, Visible = false, LinkColor = Color.FromArgb(34, 91, 73) };
    readonly ProgressBar progress = new() { Style = ProgressBarStyle.Marquee, Dock = DockStyle.Fill, Visible = false, Height = 5, MarqueeAnimationSpeed = 35 };
    readonly List<Control> editControls = new();
    readonly List<Camera> cameras = new();
    readonly StringBuilder log = new();
    readonly OperationState activity = new();
    readonly CancellationTokenSource lifetime = new();
    readonly System.Windows.Forms.Timer connectionTimer = new() { Interval = 5000 };
    Process? capturing;
    CancellationTokenSource? cancellation;
    Task<List<Camera>>? discoveryTask;
    Task? captureStopTask;
    bool captureForcedStop;
    bool autoMp4 = true, trimPlaceholder;
    int estimatedMinutes = 60;
    string? selectedCameraId;
    string? lastFolder;
    string? lastRaw;
    string? lastCaptureRecords;
    bool busy => activity.Busy;
    static readonly string Cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DV-Tape-Exporter", "tools");
    static readonly string PreferencesPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DV-Tape-Exporter", "settings.json");
    const string FfmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";

    [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint state);

    public MainForm()
    {
        Text = "DV 磁带导出 · 0.2";
        Font = new Font("Microsoft YaHei UI", 10); BackColor = Color.White;
        ClientSize = new(700, 530); MinimumSize = new(620, 540); StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        folder.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "DV 磁带");
        LoadPreferences();
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true }; Controls.Add(scroll);
        var layout = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(30, 26, 30, 22), ColumnCount = 1, RowCount = 10 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        for (int i = 0; i < 10; i++) layout.RowStyles.Add(new(SizeType.AutoSize));
        scroll.Controls.Add(layout);
        layout.Controls.Add(new Label { Text = "把磁带保存下来", AutoSize = true, Font = new Font(Font.FontFamily, 22, FontStyle.Bold), Margin = new(0, 0, 0, 8) }, 0, 0);
        layout.Controls.Add(new Label { Text = "接好摄像机，在磁带回放模式准备好磁带。", AutoSize = true, ForeColor = Color.FromArgb(100, 110, 105), Margin = new(0, 0, 0, 18) }, 0, 1);
        layout.Controls.Add(connection, 0, 2);
        var inputs = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 2, Margin = new(0, 24, 0, 20) };
        inputs.ColumnStyles.Add(new(SizeType.Absolute, 94)); inputs.ColumnStyles.Add(new(SizeType.Percent, 100));
        inputs.Controls.Add(new Label { Text = "磁带名称", AutoSize = true, Margin = new(0, 7, 0, 18) }, 0, 0);
        tape.Margin = new(0, 0, 0, 18); inputs.Controls.Add(tape, 1, 0);
        inputs.Controls.Add(new Label { Text = "保存位置", AutoSize = true }, 0, 1);
        var path = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, Margin = Padding.Empty };
        path.ColumnStyles.Add(new(SizeType.Percent, 100)); path.ColumnStyles.Add(new(SizeType.AutoSize));
        var changeFolder = new LinkLabel { Text = "更改…", AutoSize = true, LinkColor = Color.FromArgb(34, 91, 73) };
        changeFolder.LinkClicked += (_, _) => { using var d = new FolderBrowserDialog { InitialDirectory = folder.Text }; if (d.ShowDialog(this) == DialogResult.OK) { folder.Text = d.SelectedPath; SavePreferences(); } };
        path.Controls.Add(folder, 0, 0); path.Controls.Add(changeFolder, 1, 0); inputs.Controls.Add(path, 1, 1); layout.Controls.Add(inputs, 0, 3);
        var reading = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, BackColor = Color.FromArgb(242, 246, 243), Padding = new Padding(18), Margin = new(0, 0, 0, 20), ColumnCount = 1, RowCount = 3 };
        reading.ColumnStyles.Add(new(SizeType.Percent, 100));
        reading.Controls.Add(counters, 0, 0); status.Margin = new(0, 10, 0, 8); reading.Controls.Add(status, 0, 1); reading.Controls.Add(progress, 0, 2); layout.Controls.Add(reading, 0, 4);
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Margin = Padding.Empty };
        primary.FlatAppearance.BorderSize = 0; primary.Margin = new(0, 0, 14, 0); more.FlatAppearance.BorderSize = 0; more.Margin = new(0, 8, 0, 0);
        actions.Controls.Add(primary); actions.Controls.Add(more); layout.Controls.Add(actions, 0, 5);
        resultLink.Margin = new(0, 16, 0, 0); layout.Controls.Add(resultLink, 0, 6);
        layout.Controls.Add(new Label { Text = "一小时磁带约需一小时采集。原始录像始终保留。", AutoSize = true, ForeColor = Color.FromArgb(100, 110, 105), Margin = new(0, 22, 0, 0) }, 0, 7);
        editControls.AddRange(new Control[] { tape, changeFolder, more });
        layout.SizeChanged += (_, _) => { int width = Math.Max(280, layout.ClientSize.Width - layout.Padding.Horizontal - 45); status.MaximumSize = connection.MaximumSize = new(width, 0); };
        primary.Click += async (_, _) => { if (busy) await RequestStop(); else await Guard(StartCapture); };
        resultLink.LinkClicked += (_, _) => OpenSavedFolder();
        more.Click += (_, _) => menu.Show(more, new Point(0, more.Height));
        menu.Items.Add(chooseCamera);
        var convert = new ToolStripMenuItem("把已有录像转换为 MP4…"); convert.Click += async (_, _) => await Guard(ExportExisting, WorkPhase.Converting); menu.Items.Add(convert);
        var components = new ToolStripMenuItem("下载播放文件组件"); components.Click += async (_, _) => await Guard(DownloadComponents, WorkPhase.Other); menu.Items.Add(components);
        var rewind = new ToolStripMenuItem("倒带到开头"); rewind.Click += async (_, _) => await Guard(Rewind); menu.Items.Add(rewind);
        menu.Items.Add(new ToolStripSeparator());
        var options = new ToolStripMenuItem("保存选项…"); options.Click += (_, _) => ShowOptions(); menu.Items.Add(options);
        var records = new ToolStripMenuItem("查看运行记录…"); records.Click += (_, _) => ShowLog(); menu.Items.Add(records);
        var open = new ToolStripMenuItem("打开保存文件夹"); open.Click += (_, _) => OpenSavedFolder(); menu.Items.Add(open);
        menu.Items.Add(confirmEnd);
        menu.Items.Add(new ToolStripSeparator());
        var help = new ToolStripMenuItem("连接帮助"); help.Click += (_, _) => { var p = Path.Combine(AppContext.BaseDirectory, "使用说明.html"); if (File.Exists(p)) Process.Start(new ProcessStartInfo(p) { UseShellExecute = true }); else MessageBox.Show(this, "请阅读压缩包内的使用说明。", "连接帮助"); }; menu.Items.Add(help);
        confirmEnd.Click += (_, _) =>
        {
            if (busy || lastCaptureRecords == null) return;
            if (MessageBox.Show(this, "确认对象：" + lastRaw + "\n\n请在摄像机上核实：磁带已从开头正常播放至卷尾，且已采集到最后的录像。\n\n确定记录你已确认到卷尾吗？这不会修改录像，也不会把后端的普通停止状态改成 END 信号。", "记录你的卷尾确认", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try { Media.WriteNewJson(Path.Combine(lastCaptureRecords, "用户卷尾确认.json"), new { physical_end_confirmed_by_user = true, source = "user_verified_camera_and_tape_position", confirmed_at = DateTimeOffset.Now, original_backend_record_unchanged = true }); Status("已记录你确认磁带播放到卷尾。录像文件保留。"); confirmEnd.Enabled = false; }
            catch (IOException e) { MessageBox.Show(this, e.Message, "卷尾确认"); }
        };
        FormClosing += async (_, e) =>
        {
            if (!busy) { SavePreferences(); return; }
            e.Cancel = true;
            Status("正在结束当前任务。保存完成后窗口会关闭。"); await RequestStop(closeWindow: true);
        };
        FormClosed += (_, _) => { connectionTimer.Stop(); connectionTimer.Dispose(); lifetime.Cancel(); lifetime.Dispose(); menu.Dispose(); };
        Shown += async (_, _) => { connectionTimer.Start(); await PollConnection(); };
        connectionTimer.Tick += async (_, _) => await PollConnection();
        AcceptButton = primary;
    }
    void Status(string text)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) { BeginInvoke(() => Status(text)); return; }
        status.Text = text;
    }
    void Note(string text)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) { BeginInvoke(() => Note(text)); return; }
        log.AppendLine($"[{DateTime.Now:HH:mm:ss}] {text}");
        if (log.Length > 200000) log.Remove(0, log.Length - 150000);
    }
    void SyncUi()
    {
        primary.Text = activity.PrimaryText; primary.Enabled = activity.CanUsePrimary;
        primary.BackColor = activity.Phase == WorkPhase.Capturing ? Color.FromArgb(153, 67, 55) : Color.FromArgb(34, 91, 73);
        foreach (var c in editControls) c.Enabled = !busy;
        resultLink.Enabled = !busy;
    }
    async Task Guard(Func<Task> work, WorkPhase phase = WorkPhase.Preparing)
    {
        if (busy) return; activity.Begin(phase); captureStopTask = null; captureForcedStop = false; cancellation = new();
        SyncUi(); SavePreferences();
        progress.Visible = true; SetThreadExecutionState(0x80000001);
        try { await work(); }
        catch (OperationCanceledException) { Status("已停止。已采集的原始录像保留；未完成的播放文件不能作为最终结果。"); Note("任务已停止，原始文件不会删除。"); }
        catch (Exception e) { Status(e.Message); Note("需要检查：" + e.Message); if (!activity.CloseAfterWork) MessageBox.Show(this, e.Message, "DV 磁带导出", MessageBoxButtons.OK, MessageBoxIcon.Information); }
        finally
        {
            try
            {
            if (capturing is { HasExited: false }) await DrainCapture(capturing);
            if (captureStopTask != null) await captureStopTask;
            }
            catch (Exception e) { Note("采集收尾需要检查：" + e.Message); Status("原始文件已保留，请查看保存记录检查收尾结果。"); }
            finally
            {
            capturing?.Dispose(); capturing = null; cancellation.Dispose(); cancellation = null;
            activity.End(); progress.Visible = false; SyncUi();
            SetThreadExecutionState(0x80000000);
            }
            if (activity.CloseAfterWork) Close();
        }
    }
    Task StopCaptureProcess(Process process) => captureStopTask ??= StopCaptureCore(process);
    async Task StopCaptureCore(Process process)
    {
        Status("正在保存已收到的录像并关闭采集连接…");
        captureForcedStop |= await CaptureShutdown.Stop(process, TimeSpan.FromSeconds(30));
        if (captureForcedStop) Note("采集组件未能正常退出，已强制结束；当前原始文件需检查，不能认作完整成功。");
    }
    async Task DrainCapture(Process process)
    {
        // The normal output reader has already left its loop when this runs.
        var remaining = process.StandardOutput.ReadToEndAsync();
        await StopCaptureProcess(process); await remaining;
    }
    string Exe(string mode) => Path.Combine(AppContext.BaseDirectory, mode == "directshow" ? "directshow" : "native", mode == "directshow" ? "dv-directshow.exe" : "uvcdv-capture.exe");
    Camera Selected => cameras.FirstOrDefault(x => x.Id == selectedCameraId) ?? throw new IOException(cameras.Count > 1 ? "发现多台摄像机，请在“更多 → 选择摄像机”中选择要采集的一台。" : "没有找到摄像机。请检查 USB 连接、磁带回放和 MotionDV 模式。");
    async Task<List<Camera>> DiscoverCameras()
    {
        const string mode = "directshow";
        string exe = Exe(mode); if (!File.Exists(exe)) throw new IOException("采集组件缺失。请完整解压软件，不要单独移动 EXE。");
        var found = new List<Camera>();
        using var p = new Process { StartInfo = Media.Command(exe, new[] { "--list-json" }) };
        p.Start(); p.StandardInput.Close(); var errTask = p.StandardError.ReadToEndAsync();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token); timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var terminate = timeout.Token.Register(() => { try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) { } });
        try
        {
        while (await p.StandardOutput.ReadLineAsync(timeout.Token) is { } line)
        {
            if (!line.StartsWith('{')) continue;
            using var doc = JsonDocument.Parse(line); var d = doc.RootElement;
            if (d.TryGetProperty("type", out var t) && t.GetString() == "device")
            {
                var id = d.TryGetProperty("id", out var idNode) ? idNode.GetString() : null;
                bool supported = !d.TryGetProperty("supported", out var s) || s.GetBoolean();
                if (supported && id != null) found.Add(new Camera(id, d.TryGetProperty("name", out var n) ? n.GetString() ?? id : id, mode));
            }
        }
        await p.WaitForExitAsync(timeout.Token); var err = await errTask;
        if (err.Length > 0) Note(err.Trim());
        if (p.ExitCode != 0) throw new IOException("无法检查摄像机连接，请在“更多 → 查看运行记录”中查看详情。");
        return found;
        }
        finally
        {
            try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) { }
            await p.WaitForExitAsync(); await errTask;
        }
    }
    async Task RefreshConnection(bool duringWork = false)
    {
        if (discoveryTask == null || discoveryTask.IsCompleted) discoveryTask = DiscoverCameras();
        var found = await discoveryTask.WaitAsync(duringWork ? cancellation!.Token : lifetime.Token);
        if (IsDisposed || Disposing || (!duringWork && (busy || menu.Visible))) return;
        cameras.Clear(); cameras.AddRange(found);
        if (cameras.Count == 1) selectedCameraId = cameras[0].Id;
        else if (!cameras.Any(x => x.Id == selectedCameraId)) selectedCameraId = null;
        foreach (ToolStripItem item in chooseCamera.DropDownItems.Cast<ToolStripItem>().ToArray()) item.Dispose();
        chooseCamera.DropDownItems.Clear(); chooseCamera.Visible = cameras.Count > 1;
        foreach (var camera in cameras.Count > 1 ? cameras : Enumerable.Empty<Camera>())
        {
            var item = new ToolStripMenuItem(camera.Name) { Checked = camera.Id == selectedCameraId };
            item.Click += (_, _) => { selectedCameraId = camera.Id; foreach (ToolStripMenuItem other in chooseCamera.DropDownItems) other.Checked = other == item; ShowConnection(); };
            chooseCamera.DropDownItems.Add(item);
        }
        ShowConnection();
    }
    void ShowConnection()
    {
        var current = cameras.FirstOrDefault(x => x.Id == selectedCameraId);
        connection.Text = current != null ? "已连接 · " + current.Name : cameras.Count > 1 ? "发现多台摄像机 · 请在“更多”中选择" : "未发现摄像机 · 接好 USB 后会自动识别";
        connection.ForeColor = current != null ? Color.FromArgb(34, 91, 73) : Color.FromArgb(100, 110, 105);
    }
    async Task PollConnection()
    {
        if (busy || menu.Visible || IsDisposed || Disposing || discoveryTask is { IsCompleted: false }) return;
        try { await RefreshConnection(); }
        catch (Exception e)
        {
            if (IsDisposed || Disposing || busy) return;
            cameras.Clear(); selectedCameraId = null; chooseCamera.Visible = false;
            connection.Text = "暂未连接 · 请检查摄像机电源和 USB";
            Note("自动连接检查：" + e.Message);
        }
    }
    async Task Rewind()
    {
        await RefreshConnection(duringWork: true);
        cancellation!.Token.ThrowIfCancellationRequested();
        var camera = Selected;
        var args = camera.Backend == "directshow" ? new[] { "--tape", "rew", "--device", camera.Id } : new[] { "--vid", "0x04da", "--pid", "0x231a", "--tape", "rew" };
        using var p = new Process { StartInfo = Media.Command(Exe(camera.Backend), args) };
        p.Start(); p.StandardInput.Close(); var output = p.StandardOutput.ReadToEndAsync(); var errors = p.StandardError.ReadToEndAsync();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation!.Token); timeout.CancelAfter(TimeSpan.FromSeconds(15));
        using var terminate = timeout.Token.Register(() => { try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) { } });
        try
        {
        await p.WaitForExitAsync(timeout.Token); var o = await output; var e = await errors;
        if (p.ExitCode != 0) throw new IOException("电脑无法控制倒带，请使用摄像机上的倒带按钮。" + e);
        Note(o.Trim()); Status("已发送倒带指令。请等摄像机倒带结束，再点击开始采集。");
        }
        finally
        {
            try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) { }
            await p.WaitForExitAsync(); await output; await errors;
        }
    }
    async Task StartCapture()
    {
        Status("正在连接摄像机…");
        await RefreshConnection(duringWork: true);
        cancellation!.Token.ThrowIfCancellationRequested();
        var camera = Selected; string name = Media.SafeName(tape.Text), root = Path.GetFullPath(folder.Text);
        Directory.CreateDirectory(root);
        var drive = new DriveInfo(Path.GetPathRoot(root)!);
        long required = (long)(estimatedMinutes / 60.0 * 18_000_000_000);
        if (drive.AvailableFreeSpace < required) throw new IOException($"保存位置空间不足。预计需要约 {required / 1e9:F0} GB，请换一个位置。");
        if (drive.DriveFormat.Equals("FAT32", StringComparison.OrdinalIgnoreCase)) throw new IOException("保存位置是 FAT32，无法保存一小时的大文件。请选择 NTFS 或 exFAT。");
        string session = Path.Combine(root, name + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..6]);
        Directory.CreateDirectory(session); lastFolder = session; string raw = Path.Combine(session, name + "-原始.dv"); lastRaw = raw;
        string records = Path.Combine(session, "保存记录"); Directory.CreateDirectory(records); lastCaptureRecords = records; confirmEnd.Enabled = false;
        var args = camera.Backend == "directshow"
            ? new[] { "capture", "--device", camera.Id, "--output", raw, "--stop-on-stdin", "--max-sec", "10800", "--play-after-stream", "--stop-on-finish" }
            : new[] { "--vid", "0x04da", "--pid", "0x231a", "--monitor-tape", "--status-json", "--stop-on-stdin", "--play-after-stream", "--stop-after-camera-stop", "20", "--stop", "--max-sec", "10800", raw };
        var process = new Process { StartInfo = Media.Command(Exe(camera.Backend), args) };
        using var outputLog = new StreamWriter(new FileStream(Path.Combine(records, "采集输出.txt"), FileMode.CreateNew), new UTF8Encoding(false));
        using var errorLog = new StreamWriter(new FileStream(Path.Combine(records, "采集诊断.txt"), FileMode.CreateNew), new UTF8Encoding(false));
        try { process.Start(); } catch { process.Dispose(); throw; }
        capturing = process; activity.CaptureStarted(); SyncUi(); resultLink.Visible = false; counters.Text = "00:00:00   ·   0.00 GB";
        Status("正在保存原始录像。请保持电源和 USB 连接稳定。");
        var errorTask = Task.Run(async () => { while (await process.StandardError.ReadLineAsync() is { } line) await errorLog.WriteLineAsync(line); });
        JsonElement? done = null; var started = DateTimeOffset.Now;
        try
        {
        while (await process.StandardOutput.ReadLineAsync() is { } line)
        {
            await outputLog.WriteLineAsync(line);
            if (!line.StartsWith('{')) { if (line.Length > 0) Note(line); continue; }
            using var doc = JsonDocument.Parse(line); var d = doc.RootElement;
            if (!d.TryGetProperty("type", out var t)) continue;
            if (t.GetString() == "done") done = d.Clone();
            else if (t.GetString() == "status")
            {
                long b = d.TryGetProperty("bytes", out var bn) ? bn.GetInt64() : 0;
                double elapsed = Media.ElapsedSeconds(d);
                counters.Text = $"{TimeSpan.FromSeconds(elapsed):hh\\:mm\\:ss}   ·   {b / 1e9:F2} GB";
            }
            else if ((t.GetString() == "play" && d.TryGetProperty("set_result", out var r) && r.GetInt32() != 1) || (t.GetString() == "play_result" && d.TryGetProperty("result", out var pr) && pr.GetInt32() != 1))
                { Note("自动播放未被摄像机接受，请在摄像机上按播放。"); Status("请在摄像机上按播放，电脑会继续保存录像。"); }
            else if (t.GetString() == "transport" && d.TryGetProperty("action", out var action) && action.GetString() == "play" && d.TryGetProperty("command_accepted", out var accepted) && !accepted.GetBoolean())
                { Note("自动播放未被摄像机接受，请在摄像机上按播放。"); Status("请在摄像机上按播放，电脑会继续保存录像。"); }
        }
        await process.WaitForExitAsync();
        }
        finally { if (!process.HasExited) await DrainCapture(process); if (captureStopTask != null) await captureStopTask; await errorTask; await outputLog.FlushAsync(); await errorLog.FlushAsync(); }
        int rc = process.ExitCode; long actual = File.Exists(raw) ? new FileInfo(raw).Length : 0;
        var outcome = Media.AssessCapture(done, rc, actual);
        if (captureForcedStop) outcome = outcome with { Saved = false, ZeroLossVerified = false };
        bool agreement = outcome.BytesAgree;
        bool physicalEnd = outcome.PhysicalEndVerified;
        string scope = physicalEnd ? "physical_end_reported_by_backend" : "stopped_unconfirmed_tape_end";
        Media.WriteNewJson(Path.Combine(records, "采集记录.json"), new { tape_label = name, backend = camera.Backend, camera = camera.Name, raw, started, finished = DateTimeOffset.Now, exit_code = rc, forced_termination = captureForcedStop, file_bytes = actual, writer_bytes_agree = agreement, received_data_saved = outcome.Saved, zero_transport_loss_verified = outcome.ZeroLossVerified, physical_end_verified = physicalEnd, completion_scope = scope, backend_result = done, quality_note = "Raw DV retains camera error-flagged data. File integrity does not establish source image quality; unavailable drop counters remain null." });
        confirmEnd.Enabled = outcome.Saved && !physicalEnd;
        if (actual > 0) { Note($"原始录像已保留：{raw}"); resultLink.Visible = true; }
        if (!outcome.Saved) throw new IOException(actual > 0 ? "采集被中断或发现传输/写入问题，已收到的原始录像仍保留。请查看保存记录，不能把这次采集认作完整成功。" : "没有收到可保存的录像数据。请检查连接与磁带播放状态，详细诊断已保存在本次文件夹中。");
        Note(physicalEnd ? "后端报告已到磁带末尾。" : "已保存至停止位置，物理卷尾尚未确认。请查看摄像机屏幕。");
        capturing.Dispose(); capturing = null;
        var tools = ComponentPaths();
        if (activity.BeginConversionAfterCapture(outcome.Saved, autoMp4, tools.HasValue))
        {
            SyncUi();
            await Media.Export(tools!.Value.Ffmpeg, tools.Value.Ffprobe, raw, Path.Combine(session, name + ".mp4"), trimPlaceholder, records, cancellation!.Token, Status);
            Note("播放文件整段检查通过。");
        }
        else if (autoMp4 && !tools.HasValue && !activity.CloseAfterWork)
        { Status("原始录像已保存。在“更多”中准备播放组件后，即可转换为 MP4。"); return; }
        Status(physicalEnd ? "保存完成，已报告磁带末尾。" : "已保存至停止位置。磁带是否到卷尾，请以摄像机为准。");
    }
    async Task RequestStop(bool closeWindow = false)
    {
        var action = activity.RequestStop(closeWindow); SyncUi();
        if (action == StopAction.StopCapture && capturing != null)
        {
            Status("正在停止并写完已收到的录像，请稍候…");
            try { await StopCaptureProcess(capturing); }
            catch (Exception e) { Note("停止采集：" + e.Message); Status("正在结束采集连接，请稍候…"); }
        }
        else if (action == StopAction.CancelTask) { primary.Enabled = false; Status("正在取消当前操作，原始录像保留…"); cancellation?.Cancel(); }
    }
    (string Ffmpeg, string Ffprobe)? ComponentPaths()
    {
        foreach (var root in new[] { Path.Combine(AppContext.BaseDirectory, "tools"), Cache })
        {
            if (!Directory.Exists(root)) continue;
            foreach (var f in Directory.EnumerateFiles(root, "ffmpeg.exe", SearchOption.AllDirectories))
            { string probe = Path.Combine(Path.GetDirectoryName(f)!, "ffprobe.exe"); if (File.Exists(probe)) return (f, probe); }
        }
        return null;
    }
    async Task DownloadComponents()
    {
        if (ComponentPaths() != null) { Status("播放文件组件已就绪。"); return; }
        Status("正在从 FFmpeg 官网列出的 Windows 构建站点下载组件…");
        Directory.CreateDirectory(Cache); var zip = Path.Combine(Cache, "ffmpeg-" + Guid.NewGuid().ToString("N") + ".zip");
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(15) };
        string checksum = (await http.GetStringAsync(FfmpegUrl + ".sha256", cancellation!.Token)).Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)[0];
        if (checksum.Length != 64 || checksum.Any(x => !Uri.IsHexDigit(x))) throw new IOException("下载站点未提供有效校验值，请稍后重试。");
        using (var response = await http.GetAsync(FfmpegUrl, HttpCompletionOption.ResponseHeadersRead, cancellation.Token))
        {
            response.EnsureSuccessStatusCode(); await using var input = await response.Content.ReadAsStreamAsync(cancellation.Token);
            await using var output = new FileStream(zip, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20, true);
            var buffer = new byte[1 << 20]; long received = 0; int n;
            while ((n = await input.ReadAsync(buffer, cancellation.Token)) > 0)
            { await output.WriteAsync(buffer.AsMemory(0, n), cancellation.Token); received += n; Status($"正在下载播放组件：{received / 1e6:F0} MB"); }
        }
        using (var stream = File.OpenRead(zip))
            if (!Convert.ToHexString(await SHA256.HashDataAsync(stream, cancellation.Token)).Equals(checksum, StringComparison.OrdinalIgnoreCase)) throw new IOException("组件下载校验未通过，未运行下载文件。请重试。");
        var dest = Path.Combine(Cache, "ffmpeg-" + DateTime.Now.ToString("yyyyMMdd") + "-" + Guid.NewGuid().ToString("N")[..6]);
        // ExtractToDirectory rejects path traversal; executable paths come only from the verified archive.
        ZipFile.ExtractToDirectory(zip, dest); File.Delete(zip);
        Media.WriteNewJson(Path.Combine(dest, "download-record.json"), new { url = FfmpegUrl, sha256 = checksum, fetched = DateTimeOffset.Now, provider = "gyan.dev, linked by ffmpeg.org/download.html", licenses = "Included provider LICENSE and README in extracted archive" });
        if (ComponentPaths() == null) throw new IOException("下载完成但未找到转换组件，请查看连接帮助。");
        Status("播放文件组件已就绪。"); Note("下载校验通过，组件仅用于电脑上的视频转换。");
    }
    async Task ExportExisting()
    {
        var tools = ComponentPaths() ?? throw new IOException("请先点击“下载播放文件组件”。原始录像采集不需要此组件。");
        using var dialog = new OpenFileDialog { Filter = "原始 DV 录像|*.dv", Title = "选择已经采集完成的原始录像", FileName = lastRaw ?? "" };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        using var save = new SaveFileDialog { Filter = "MP4 播放文件|*.mp4", FileName = Path.GetFileNameWithoutExtension(dialog.FileName).Replace("-原始", "") + ".mp4", InitialDirectory = Path.GetDirectoryName(dialog.FileName), OverwritePrompt = false };
        if (save.ShowDialog(this) != DialogResult.OK) return;
        if (File.Exists(save.FileName)) throw new IOException("这个播放文件已存在，请换一个名称。软件不会覆盖它。");
        string records = save.FileName + "-记录-" + Guid.NewGuid().ToString("N")[..6]; Directory.CreateDirectory(records); lastFolder = Path.GetDirectoryName(save.FileName);
        await Media.Export(tools.Ffmpeg, tools.Ffprobe, dialog.FileName, save.FileName, trimPlaceholder, records, cancellation!.Token, Status);
        resultLink.Visible = true;
        Status("MP4 已生成，整段文件检查通过。原始 DV 保留。"); Note("播放文件：" + save.FileName);
    }
    sealed record Preferences(string SaveFolder, bool AutoMp4 = true, bool TrimPlaceholder = false, int EstimatedMinutes = 60);
    void LoadPreferences()
    {
        try
        {
            if (!File.Exists(PreferencesPath) || new FileInfo(PreferencesPath).Length > 65536) return;
            var saved = JsonSerializer.Deserialize<Preferences>(File.ReadAllText(PreferencesPath));
            if (saved == null) return;
            if (!string.IsNullOrWhiteSpace(saved.SaveFolder) && Path.IsPathFullyQualified(saved.SaveFolder)) folder.Text = saved.SaveFolder;
            autoMp4 = saved.AutoMp4; trimPlaceholder = saved.TrimPlaceholder; estimatedMinutes = Math.Clamp(saved.EstimatedMinutes, 10, 180);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { Note("使用默认保存选项：" + e.Message); }
    }
    void SavePreferences()
    {
        string temp = PreferencesPath + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(PreferencesPath)!);
            File.WriteAllText(temp, JsonSerializer.Serialize(new Preferences(folder.Text, autoMp4, trimPlaceholder, estimatedMinutes), Media.JsonOptions), new UTF8Encoding(false));
            File.Move(temp, PreferencesPath, overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Note("保存选项未能记住：" + e.Message); }
        finally { try { if (File.Exists(temp)) File.Delete(temp); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
    }
    void OpenSavedFolder()
    {
        var path = lastFolder ?? folder.Text;
        if (Directory.Exists(path)) Process.Start(new ProcessStartInfo("explorer.exe") { UseShellExecute = true, ArgumentList = { path } });
    }
    void ShowOptions()
    {
        using var dialog = new Form { Text = "保存选项", Font = Font, ClientSize = new(580, 285), FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false, StartPosition = FormStartPosition.CenterParent, AutoScaleMode = AutoScaleMode.Dpi };
        var panel = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding = new Padding(20) };
        dialog.Controls.Add(panel);
        var mp4 = new CheckBox { Text = "保存原始录像后，自动生成 MP4 播放文件", AutoSize = true, Checked = autoMp4, Margin = new(0, 0, 0, 16) };
        var trim = new CheckBox { Text = "去掉末尾已确认的空白占位画面", AutoSize = true, Checked = trimPlaceholder };
        panel.Controls.Add(mp4); panel.Controls.Add(trim);
        panel.Controls.Add(new Label { Text = "原始 DV 始终完整保留。普通静止画面不自动删除。", AutoSize = true, MaximumSize = new(530, 0), ForeColor = Color.DimGray, Margin = new(20, 4, 0, 16) });
        var time = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        var minutes = new NumericUpDown { Minimum = 10, Maximum = 180, Value = estimatedMinutes, Width = 78 };
        time.Controls.Add(new Label { Text = "预计磁带时长", AutoSize = true, Margin = new(0, 5, 10, 0) }); time.Controls.Add(minutes); time.Controls.Add(new Label { Text = "分钟", AutoSize = true, Margin = new(6, 5, 0, 0) }); panel.Controls.Add(time);
        panel.Controls.Add(new Label { Text = "仅用于空间检查，不会到时自动停止。默认按一小时预留 18 GB。", AutoSize = true, MaximumSize = new(530, 0), ForeColor = Color.DimGray, Margin = new(0, 6, 0, 16) });
        var buttons = new FlowLayoutPanel { AutoSize = true }; var save = new Button { Text = "保存", DialogResult = DialogResult.OK, AutoSize = true }; var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, AutoSize = true };
        buttons.Controls.Add(save); buttons.Controls.Add(cancel); panel.Controls.Add(buttons); dialog.AcceptButton = save; dialog.CancelButton = cancel;
        if (dialog.ShowDialog(this) == DialogResult.OK) { autoMp4 = mp4.Checked; trimPlaceholder = trim.Checked; estimatedMinutes = (int)minutes.Value; SavePreferences(); }
    }
    void ShowLog()
    {
        using var dialog = new Form { Text = "运行记录", Font = Font, ClientSize = new(720, 440), StartPosition = FormStartPosition.CenterParent, AutoScaleMode = AutoScaleMode.Dpi };
        var text = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Dock = DockStyle.Fill, Text = log.ToString() };
        dialog.Controls.Add(text); dialog.ShowDialog(this);
    }
}
