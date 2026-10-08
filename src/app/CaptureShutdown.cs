// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;

namespace DvTape;

/// <summary>
/// Requests capture shutdown through stdin, then waits independently for exit.
/// The existing process owner continues to drain stdout and stderr. Callers
/// should share one invocation per capture and retain the process until it ends.
/// </summary>
public static class CaptureShutdown
{
    /// <returns>True if a timed-out child required a process-tree kill.</returns>
    public static async Task<bool> Stop(Process process, TimeSpan timeout)
    {
        ArgumentNullException.ThrowIfNull(process);
        if (process.HasExited) return false;

        StreamWriter? input = null;
        try
        {
            input = process.StandardInput;
            await input.WriteLineAsync("stop");
            await input.FlushAsync();
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
        {
            // A closed/broken input pipe must not bypass waiting for the child.
        }
        finally
        {
            try { input?.Close(); }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException) { }
        }

        Task exited = process.WaitForExitAsync();
        try
        {
            await exited.WaitAsync(timeout);
            return false;
        }
        catch (TimeoutException)
        {
            // The child may have exited exactly as the timer elapsed.
            if (process.HasExited) { await exited; return false; }
            try { process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) when (process.HasExited)
            {
                await exited;
                return false;
            }
            await exited;
            return true;
        }
    }
}
