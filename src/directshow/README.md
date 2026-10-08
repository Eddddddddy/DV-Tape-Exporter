# Windows native DV backend

`dv-directshow.exe` is the default native-driver backend for Windows x64 (Windows 11 recommended).
It uses .NET 10 COM interop and Windows DirectShow, without installing a driver,
replacing an interface driver, or changing USB configuration. Source is licensed
GPL-3.0-or-later, consistently with the enclosing application.

This backend has been cross-compiled on macOS and its managed file pipeline has
passed offline checks. **Windows device enumeration, COM graph negotiation, tape
control and real NV-GS28 capture have not been tested on Windows.** A build is
not evidence that a particular Windows driver exposes this camera's raw DV pin.

## CLI contract

```text
dv-directshow.exe --list-json
dv-directshow.exe capture --device "MONIKER" --output "D:\录像\磁带.dv" --stop-on-stdin --max-sec 10800 --play-after-stream --stop-on-finish
dv-directshow.exe --device "MONIKER" --tape status
dv-directshow.exe --device "MONIKER" --tape rew
dv-directshow.exe --device "MONIKER" --tape play
dv-directshow.exe --device "MONIKER" --tape stop
```

Stdout is newline-delimited UTF-8 JSON. Pass a device ID back unchanged; it is
the DirectShow moniker display name, not a user-visible friendly name.

Enumeration binds matching devices and reads pin metadata. It never runs the
graph or writes transport controls. Only devices whose moniker/device path has
`vid_04da` and `pid_231a`, or whose friendly name explicitly contains Panasonic
or Matsushita, are inspected. A `device` record is emitted only for an output
pin whose category is `PIN_CATEGORY_CAPTURE`, subtype is `dvsd`, and major type
is Video or Interleaved. Other webcams and the NV-GS28's USB storage mode are
not returned as supported devices. Pin-category queries that are unavailable
fail closed. This can miss a driver that exposes raw DV but omits category
metadata; the application must then report native support as unavailable.

```json
{"type":"device","id":"@device:pnp:...","name":"Panasonic ...","supported":true,"backend":"directshow","subtype":"dvsd","native_driver":true}
{"type":"list_done","backend":"directshow","devices":1}
```

An unsupported matching device produces an `info` record with `supported:false`.
The GUI should include only `type == "device" && supported == true`. `transport`
in a device record means the optional COM interface exists, not that each
individual method is implemented by its driver.

Capture saves `.dv` media bytes exactly as delivered to the sample callback.
The output uses atomic `FileMode.CreateNew`; existing files are never truncated,
overwritten or automatically renamed. Filenames are Unicode. Failed runs retain
their partial file. An output file may contain an incomplete final DV frame;
the later audit must report it and must not repair the original in place.

The graph is exactly:

```text
Panasonic capture pin (dvsd) → Sample Grabber → Null Renderer
                              ↓ buffer copy
                           CreateNew .dv
```

Both connections use `ConnectDirect`. No Intelligent Connect, decoder, splitter,
audio extraction, mux or video renderer is inserted. The negotiated input is
checked again for raw `dvsd`. SD-DV carries its audio inside the DV frame, so
this preserves embedded audio packs, including 32 kHz 12-bit four-channel
recordings, for the application's later audio conversion.

Callbacks only copy into a bounded queue (256 samples, at most 64 MiB payload).
The writer drains all accepted buffers after graph shutdown and flushes the
file to disk. Queue overflow stops capture with `buffer_error`; disk failures
stop with `write_error`. Neither is reported as a successful complete transfer.
`drop` is always `null`: the backend has no USB packet loss counter and cannot
infer loss-free transport from successful disk writing. The known application
queue-overflow counter is reported separately as `ovf`.

`--play-after-stream` installs the file sink and starts the graph before sending
the optional playback command. It sends PLAY after the first sample or after
one second if the paused driver supplies no samples. A transport failure is
reported explicitly; manual playback can still be used. With neither playback
flag, capture does not issue tape-control commands. `--stop-on-finish` requests
STOP after graph shutdown and reports whether the driver accepted that command.

Send a line containing `stop` to stdin to stop normally. If `--stop-on-stdin` is
set, stdin EOF or a broken stdin also stops; keep the parent process's redirected
stdin open during capture. Ctrl+C stops gracefully. `--max-sec` accepts integers
1–10800, default 10800; it is a safety ceiling, not an estimate of tape length.

One status record is emitted per second:

```json
{"type":"status","backend":"directshow","bytes":3600000,"written":3600000,"elapsed":1.1,"drop":null,"ovf":0,"samples":25,"camera_state":"play","physical_end_verified":false}
```

`bytes` counts bytes seen by the callback; `accepted` in the final record counts
bytes queued for writing; final `written` is the actual file length. They should
all agree for a successful drained capture. File length remains authoritative
if an OS write fails after making a partial write.

```json
{"type":"done","backend":"directshow","bytes":14400000,"written":14400000,"accepted":14400000,"physical_end_verified":false,"stop_reason":"user_stop","device_lost":false,"drop":null,"write_error":0,"partial":false,"success":true,"camera_stopped":true,"end":"unconfirmed"}
```

`success` means that a nonempty file was saved without a detected capture/write
failure. It does **not** mean an entire tape or intact source pictures were
verified. `partial` means a detected failure or empty file; user-stopped captures
can still represent only part of a tape. The UI must preserve these distinctions.
Possible stop reasons include `user_stop`, `stdin_eof`, `stdin_closed`,
`max_duration`, `device_lost`, `graph_error`, `stream_ended_unconfirmed`,
`buffer_error`, `write_error`, `setup_error`, and `capture_error`.

DirectShow device-removal and graph-error events trigger shutdown and queue
draining. A source `EC_COMPLETE`, ordinary tape STOP, no changing pictures,
or a duration limit does not establish physical end of tape. This backend
currently has **no positive physical-EOT proof**, so it always emits
`physical_end_verified:false` and `end:"unconfirmed"`. A paused or stopped
camera may continue sending DV frames; tape STOP is informational and does not
by itself terminate the raw capture.

Tape commands use the optional `IAMExtTransport` interface. Missing interfaces
and failed HRESULTs produce `supported:false` and exit 4, never a false success.
Only these SDK values are permitted by the command mapping:

| Command | SDK constant | Value |
|---|---|---:|
| play | ED_MODE_PLAY = ED_BASE + 200 | 4296 |
| stop | ED_MODE_STOP = ED_BASE + 201 | 4297 |
| rew | ED_MODE_REW = ED_BASE + 205 | 4301 |

Rewind acceptance does not mean rewind completion; poll `--tape status` and
wait for camera STOP, then check the camera's start position before capture.
No recording, record-pause, format, seek or edit command is exposed.

Exit codes: 0 = enumeration/control succeeded or nonempty capture saved without
a detected failure; 2 = command/capture failure or empty capture; 3 = a Windows
operation requested on another OS; 4 = optional tape control unsupported.

## Build and offline checks

```text
dotnet publish DvDirectShow.csproj -c Release -r win-x64 --self-contained true -o ../../bin/directshow
dotnet build DvDirectShow.csproj -c Release -p:PlatformTarget=AnyCPU -o OFFLINE_DIRECTORY
dotnet OFFLINE_DIRECTORY/dv-directshow.dll --self-test --self-test-dir SCRATCH_DIRECTORY
```

The AnyCPU build enables the hardware-free tests on a 64-bit macOS/Linux host;
the distributed Windows executable and runtime are x64. Self-test exercises
18,432,000 raw bytes, byte-for-byte SHA-256 matching after queue drain, counters,
Unicode paths, atomic no-overwrite, x64 `AM_MEDIA_TYPE` layout, SDK playback
constants, and the device identity allow-list. It creates a unique scratch file,
then deletes only that test file after success. It does not load Windows COM,
enumerate hardware, or send a transport command.

Keep `LICENSE.TXT` and `THIRD-PARTY-NOTICES.TXT` from the selected
`Microsoft.NETCore.App.Runtime.win-x64` runtime package beside the published
files when redistributing the self-contained runtime. They are included in the
current `bin/directshow` output; a clean rebuild must copy them again.

## Windows limitations and official references

- [Microsoft: USB Video Class driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview)
  documents native UVC support including DV formats and isochronous streaming.
  This motivates probing the existing native driver first; it does not prove
  Windows exposes the particular NV-GS28's raw capture pin.
- [Microsoft: DV video subtypes](https://learn.microsoft.com/en-us/windows/win32/directshow/dv-video-subtypes)
  defines consumer SD-DV `dvsd`.
- [Microsoft: About Digital Video in DirectShow](https://learn.microsoft.com/en-us/windows/win32/directshow/about-digital-video-in-directshow)
  describes audio and video carried together in DV frames.
- [Microsoft: ConnectDirect](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-ifiltergraph-connectdirect)
  connects pins without intervening filters.
- [Microsoft: Using the Sample Grabber](https://learn.microsoft.com/en-us/windows/win32/directshow/using-the-sample-grabber)
  describes the transform and Null Renderer graph. Microsoft marks this as a
  legacy API that may be unavailable in future Windows versions. Its documented
  format exclusions include VideoInfo2. Class-not-registered or a refused raw
  connection results in a clear failure; there is no automatic decoding fallback.
- [Microsoft: SetCallback](https://learn.microsoft.com/en-us/windows/win32/directshow/isamplegrabber-setcallback)
  defines `BufferCB` selection and warns that callbacks block the streaming
  thread. The filter does not call callbacks for preroll/non-media samples;
  this backend saves callback-delivered media and does not claim bus-level
  completeness. A real Windows capture must still be audited.
- [Microsoft: File Writer filter](https://learn.microsoft.com/en-us/windows/win32/directshow/file-writer-filter)
  accepts Stream/null, uses timestamps as file offsets, and overwrites existing
  files. We use a raw callback and CreateNew file instead of that filter.
- [Microsoft: IAMExtTransport](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-iamexttransport)
  describes optional driver support and methods that can return E_NOTIMPL.
- [Microsoft: put_Mode](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-iamexttransport-put_mode)
  defines transport commands and reports device refusal via HRESULT.
- [Microsoft: EC_DEVICE_LOST](https://learn.microsoft.com/en-us/windows/win32/directshow/ec-device-lost)
  documents removal (`lParam2 == 0`) and the need to rebuild after reconnecting.
- [Microsoft SDK strmif.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/strmif.h),
  [control.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/control.h),
  [xprtdefs.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/xprtdefs.h),
  and [uuids.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/shared/uuids.h)
  were used to check interface slots, GUIDs, data layouts and ED_BASE constants.

If the native driver provides no supported raw DV pin, the GUI should explain
this and offer the advanced libusb backend. Driver replacement must be a
separate explicit user choice; this backend does not perform it.
