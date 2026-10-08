# Synthetic UI lifecycle checks

This standalone .NET 10 console checker links the app's operation state, capture shutdown helper, and media assessment. It does not run WinForms, FFmpeg, USB, COM, or real media. Its child process reuses the checker DLL and writes a fixed binary fixture, never a private recording.

Install the .NET 10 SDK, then run from the repository root:

```sh
dotnet run --project src/ui-tests/DV.Ui.Lifecycle.Tests.csproj -c Release -- --work-root .artifacts/ui-lifecycle-tests
```

The same command works in PowerShell, macOS and Linux shells. Build artifacts stay under the repository's `.artifacts/ui-lifecycle-tests-build` directory. Each run creates a fresh GUID directory under `.artifacts/ui-lifecycle-tests`, retaining its synthetic sources, stdout/stderr logs, individual assertions, and `summary.json`. No source or derivative is deleted. The `.artifacts` directory is ignored by Git.

To choose a different results directory, change the `--work-root` argument. To choose a different build directory, pass `-p:LifecycleWorkRoot=BUILD-DIRECTORY/` to `dotnet run` or `dotnet build`.

The checks cover manual capture stop followed by conversion, independently cancellable conversion, close during capture/finalization, natural-exit stop races, stdin stop plus EOF, an already closed stdin pipe, a child that ignores stdin/EOF, byte and SHA-256 preservation, and exclusive output creation. Both output streams are continuously drained by the test owner; the shutdown helper owns neither reader.
