# Contributing

Use .NET SDK 10 and follow [the build instructions](src/BUILD.md). Keep capture behavior and MP4 conversion independently verifiable. Never discard raw DV to improve a playback copy, treat a normal stop as a verified physical tape end, or fill unavailable transport counters with zero.

Run the existing core and lifecycle checks from the repository root:

```powershell
dotnet run --project src/tests/DV.Core.Tests.csproj -c Release
dotnet run --project src/ui-tests/DV.Ui.Lifecycle.Tests.csproj -c Release -- --work-root .artifacts/ui-lifecycle-tests
```

Windows CI also builds the desktop application and runs the DirectShow backend's hardware-free self-test. These checks do not establish camera compatibility. Use only synthetic fixtures for automated tests; report hardware testing separately with model, connection mode and OS version.

Before opening a pull request or issue, remove recordings, settings, logs, credentials, device serial numbers and workstation paths. Build artifacts belong in ignored directories. Include third-party attribution and license notices when modifying a backend or assembling a release.
