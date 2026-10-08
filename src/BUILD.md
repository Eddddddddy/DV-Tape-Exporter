# Building DV Tape Exporter 0.2.0

The delivered executables target Windows x64 and .NET 10. The release build uses .NET SDK 10.0.401, libusb 1.0.30 and llvm-mingw 20260922 UCRT. A successful cross-build or offline test is not a Windows device compatibility test.

## Application behavior

The GUI automatically polls the native DirectShow backend every five seconds while idle and checks again before capture. Discovery is paused during capture and conversion. One supported camera is selected automatically; multiple cameras require a selection under the More menu. The main window contains the tape name, save location, elapsed time, received bytes and a single Start / Stop-and-save button. Secondary actions live in More. Automatic MP4 conversion is enabled by default; placeholder trimming is disabled by default. Estimated tape duration is used for storage checks only. Capture has a three-hour protection limit. Save location and options are persisted in `%LocalAppData%\DV-Tape-Exporter\settings.json`.

Stopping capture drains received data before conversion. Cancelling conversion retains the raw DV. Closing the window drains capture and exits without starting a new conversion. A missing conversion component leaves the raw file available for a later export. Device stop, manual stop, disconnection and timeout must not be described as a verified physical tape end. DirectShow transport loss counters are unavailable and remain null.

## Build on Windows

Install .NET SDK 10, then run from this source directory:

```powershell
./build-windows.ps1
```

This publishes the GUI and DirectShow backend as self-contained Windows x64 applications. Keep the runtime files beside the executables; the distribution is not a single-file application. The script also copies the HTML help to the application output directory.

Preserve all project and upstream licenses. For a complete release, include the LICENSE and THIRD-PARTY-NOTICES files from the matching Microsoft.NETCore.App.Runtime.win-x64 and Microsoft.WindowsDesktop.App.Runtime.win-x64 packages in the release `licenses` directory. Publishing alone does not assemble every release notice.

The managed projects use PathMap and disable debug output for release delivery. Keep those settings so DLL CodeView records do not expose the build workstation path. Do not distribute local PDBs, user paths, capture files or private test media.

## Offline tests

```powershell
dotnet run --project tests/DV.Core.Tests.csproj -c Release
dotnet run --project directshow/DvDirectShow.csproj -- --self-test --self-test-dir TEST-DIRECTORY
dotnet run --project tests/DV.Core.Tests.csproj -c Release -- FFMPEG FFPROBE TEST.dv
dotnet run --project ui-tests/DV.Ui.Lifecycle.Tests.csproj -c Release -- --work-root ../.artifacts/ui-lifecycle-tests
```

Use a new scratch directory for the DirectShow self-test. It tests managed raw-write, queue drain, byte accounting, no-overwrite behavior, Unicode paths, COM declarations and allowed transport constants; it does not enumerate or control a real camera.

The lifecycle tests link the production OperationState and CaptureShutdown classes. Synthetic child processes exercise normal stdin stop, pipe EOF, unresponsive-child timeout and exclusive output creation, with raw byte counts and SHA-256 read back. They also cover manual stop followed by conversion, cancelling that conversion, window-close intent during capture/finalization and the natural-exit stop race. The 0.2 run passed 52 checks. These are process/state tests, not a rendered Windows UI test, and access no camera or private media.

The media fixture is a 20-frame PAL DV file with stereo PCM. The export test expects 40 progressive frames and two 48 kHz stereo AAC tracks, uses Chinese and space-containing paths, and verifies the raw source bytes remain unchanged. Validate the current conversion arguments with FFmpeg 9.0.2, including a full decode of the resulting MP4. The test suite also covers process cancellation, progress callback failures and bounded logs. Source clips must be synthetic or separately authorized; no private tape footage is distributed.

For a minimal test-only FFmpeg build, cancellation tests need lavfi input, testsrc2, the wrapped_avframe decoder and encoder, and the null muxer, in addition to normal DV / H.264 / AAC export support.

## FFmpeg compatibility and components

FFmpeg 7 or newer is required for the file-based `-/filter_complex` syntax. Version 0.2 retains the 0.1.1 fix: pass `-/filter_complex` and the UTF-8 graph file path as separate arguments. FFmpeg 9 removed the old `filter_complex_script` option. Use ProcessStartInfo.ArgumentList and UseShellExecute=false; do not pass the graph through a command shell.

A graph file keeps filter content out of the Windows command line. Subfile input arguments still repeat the raw path for each audio run; the application limits the plan to 128 runs. Those arguments remain subject to the CreateProcessW 32,767-character limit including the terminating null, so unusually long paths can still fail. Do not replace ArgumentList with CMD string concatenation.

FFmpeg is not bundled. At user request, the application downloads the essentials ZIP and its SHA-256 from gyan.dev, a Windows build provider linked by ffmpeg.org. It verifies the hash before extraction and retains the component LICENSE / README and download record. Component discovery searches the application `tools` directory and `%LocalAppData%\DV-Tape-Exporter\tools` recursively; ffmpeg.exe and ffprobe.exe must be in the same directory. System PATH is not used for component discovery.

The required feature set includes DV video and PCM audio decoding, file / pipe / subfile inputs, bwdif, concat, resampling / padding / silence filters, libx264, AAC, MP4 output, H.264 / AAC decode verification and ffprobe JSON output. Keep the complete upstream component notices when preparing an offline tools directory.

## Capture backends and driver boundaries

DirectShow is the default GUI path. It selects a raw SD `dvsd` capture pin and uses explicit ConnectDirect connections through a Sample Grabber to a Null Renderer. IntelligentConnect is not used to insert a decoder. Samples are written unchanged to a newly created raw file. The raw DV retains embedded sound; no audio stream is discarded by this capture graph. Optional tape commands are restricted to status, play, rewind and stop. An unsupported command must be reported rather than treated as success. The backend CLI and limitations are documented in `directshow/README.md`.

The libusb executable remains an advanced compatibility fallback and is not selected automatically by the simplified GUI. Its documented JSON protocol, shutdown behavior and two-consecutive-END requirement must remain intact when rebuilding. Native tests are in `native` with CTest.

To rebuild it, obtain the [Windows x64 UCRT toolchain](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260922) and extract the official [libusb 1.0.30 Windows package](https://github.com/libusb/libusb/releases/tag/v1.0.30). CMake 3.20+ and Ninja must be available:

```powershell
./build-windows.ps1 -NativeToolchain TOOLCHAIN-DIRECTORY -LibusbDir LIBUSB-DIRECTORY
```

The Windows build uses the libusb include directory, its MinGW64 import library and the supplied libusb-1.0.dll. Preserve the DLL, LGPL notice and corresponding source in `third-party/libusb-1.0.30.tar.bz2`; upstream instructions cover rebuilding the DLL. This build script installs no driver.

WinUSB is only needed for the libusb fallback. Identify the actual video child function and its full hardware ID before any manual driver configuration. For the observed NV-GS28 MotionDV mode the VID / PID are 04DA / 231A, whereas 2316 is the storage mode. VC and VS can be grouped into one Windows function; do not hard-code MI_01. Never replace the USB Composite parent, a storage interface or an unrelated device. Record the original driver to permit restoration. Keep this information in advanced help, rather than making driver replacement a normal startup step.

References: [Microsoft UVC driver overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview), [Microsoft DirectShow DVApp sample](https://learn.microsoft.com/en-us/windows/win32/directshow/dvapp-sample), [libusb Windows notes](https://github.com/libusb/libusb/wiki/Windows), [FFmpeg downloads](https://ffmpeg.org/download.html).

## macOS cross-build

```sh
dotnet publish app/DV-Tape-Exporter.csproj -c Release -r win-x64 --self-contained true -o DESTINATION
dotnet publish directshow/DvDirectShow.csproj -c Release -r win-x64 --self-contained true -o DESTINATION/directshow
```

For the C backend, use MinGW cross CMake with CMAKE_SYSTEM_NAME=Windows, CMAKE_C_COMPILER=x86_64-w64-mingw32-clang, LIBUSB_INCLUDE_DIR and LIBUSB_LIBRARY pointing to the Windows release include and import library. Do not link macOS libusb into a Windows executable.

The packaged DirectShow executable remains x64. Running its managed offline tests on Apple Silicon requires a separate host-compatible build; do not substitute that test build into the Windows release. A locally built macOS FFmpeg 9.0.2 can validate file conversion arguments and fixtures without changing the system FFmpeg. This does not test Windows COM, GUI launch or a connected camera.

## Release assembly and upgrade

The full archive is `DV-Tape-Exporter-Windows-x64-0.2.zip`, with application version 0.2.0. Include the self-contained runtime, unchanged capture backends, help, source and notices. Exclude all captured footage and private test results. The package is unsigned.

A small upgrade from 0.1.1 updates these program files, alongside changed source, help and notices:

- DV-Tape-Exporter.exe
- DV-Tape-Exporter.dll
- DV-Tape-Exporter.deps.json

The user closes the old program and replaces these three files in the existing directory. This upgrade retains the existing .NET 10 runtime, DirectShow / native backends and downloaded components. A full archive extracted to a new directory can reuse the old application `tools` folder; per-user cached components are still discovered.

Version 0.2 has passed a Windows x64 build and offline regression checks. Its updated GUI and camera connection have not yet undergone a new Windows hardware retest. Keep that distinction in release notes. Offline correctness, saved-file integrity, source picture quality and physical tape end are separate conclusions.
