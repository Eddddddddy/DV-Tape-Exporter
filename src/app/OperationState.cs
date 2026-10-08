// SPDX-License-Identifier: GPL-3.0-or-later
namespace DvTape;

public enum WorkPhase { Idle, Preparing, Capturing, SavingCapture, Converting, Other }
public enum StopAction { None, StopCapture, CancelTask }

// A request to save the current capture is distinct from cancelling conversion.
// Closing the window drains capture without starting a new conversion task.
public sealed class OperationState
{
    public WorkPhase Phase { get; private set; }
    public bool CloseAfterWork { get; private set; }
    public bool Busy => Phase != WorkPhase.Idle;
    public string PrimaryText => Phase switch
    {
        WorkPhase.Capturing => "停止并保存",
        WorkPhase.SavingCapture => "正在保存…",
        WorkPhase.Converting => "取消转换",
        WorkPhase.Other => "取消操作",
        WorkPhase.Idle => "开始采集",
        _ => "正在处理…"
    };
    public bool CanUsePrimary => Phase is WorkPhase.Idle or WorkPhase.Capturing or WorkPhase.Converting or WorkPhase.Other;
    public void Begin(WorkPhase phase = WorkPhase.Preparing)
    {
        if (Busy || phase == WorkPhase.Idle) throw new InvalidOperationException("已有任务正在进行。");
        CloseAfterWork = false; Phase = phase;
    }
    public void CaptureStarted()
    {
        if (Phase != WorkPhase.Preparing) throw new InvalidOperationException("采集准备尚未完成。");
        Phase = WorkPhase.Capturing;
    }
    public StopAction RequestStop(bool closeWindow = false)
    {
        CloseAfterWork |= closeWindow;
        if (Phase == WorkPhase.Capturing) { Phase = WorkPhase.SavingCapture; return StopAction.StopCapture; }
        return Phase is WorkPhase.Preparing or WorkPhase.Converting or WorkPhase.Other ? StopAction.CancelTask : StopAction.None;
    }
    public bool BeginConversionAfterCapture(bool saved, bool enabled, bool componentsReady)
    {
        if (Phase is not (WorkPhase.Capturing or WorkPhase.SavingCapture)) throw new InvalidOperationException("采集尚未开始。");
        if (!saved || !enabled || !componentsReady || CloseAfterWork) return false;
        Phase = WorkPhase.Converting; return true;
    }
    public void End() => Phase = WorkPhase.Idle;
}
