# Native raw DV backend

This directory ports `uvcdv-capture` to Windows x64 with MinGW-w64/LLVM-MinGW and winpthreads. It retains the libusb callback owner-slot cancellation fix, the independent raw writer, and the original DV payload (including device-flagged data by default). It contains no recordings or device-specific private paths.

Upstream: https://github.com/boris-timofeenko/uvcdv-capture . Original copyright: Boris Timofeyenko, GPL-3.0-or-later. The local archive source was copied, then modified here; it was not changed in place. See `COPYING`. Bundled library notices are in `LIBUSB-COPYING.txt`, `WINPTHREADS-COPYING.txt`, and `MINGW-RUNTIME-COPYING.txt`; libusb source is available from https://github.com/libusb/libusb/releases/tag/v1.0.30 . Platform helper and offline test sources are distributed under the same GPL-3.0-or-later terms.

## Windows build

Use a matching architecture for the compiler, libusb import library and DLL. The supplied executable targets x64 Windows 10/11 using UCRT. WinUSB isochronous support requires Windows 8.1 or later; the application UI may require a newer OS. Use libusb 1.0.30, not a driver DLL from an unrelated package.

```sh
cmake -S src/native -B build/native -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=/path/to/llvm-mingw/bin/x86_64-w64-mingw32-clang \
  -DLIBUSB_INCLUDE_DIR=/path/to/libusb/include \
  -DLIBUSB_LIBRARY=/path/to/libusb/MinGW64/static/libusb-1.0.dll.a \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/native
```

Place `libusb-1.0.dll` from the same libusb release beside `uvcdv-capture.exe`. CMake links winpthreads statically; the bundled executable has no winpthread DLL dependency. Inspect imports again when rebuilding. `LIBUSB_LIBRARY` in the example is an import library despite the upstream folder name `static`. A truly static libusb build needs its platform dependency libraries as well.

The libusb WinUSB backend implements the Windows isochronous API. The capture application does not call WinUSB directly and does not install, replace, or reset any device driver. `--list-json` can still show descriptors when opening a candidate fails; capture requires an accessible libusb-compatible interface. A Windows build and offline test do not establish hardware compatibility.

Primary API references: https://github.com/libusb/libusb/wiki/Windows and https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_registerisochbuffer . High-speed packet counts are checked against the endpoint interval's complete USB-frame boundary; the default 128 packets satisfies interval 1's multiple-of-eight requirement.

## CLI and frontend protocol

```text
uvcdv-capture.exe --list-json
uvcdv-capture.exe --vid 0xVVVV --pid 0xPPPP --tape status-json
uvcdv-capture.exe --status-json --tape rew
uvcdv-capture.exe --status-json --stop-on-stdin --play-after-stream \
  --monitor-tape --stop-after-camera-stop 20 --stop "archive.dv"
```

The last command is an example, not an instruction to run a device test. UTF-16 Windows arguments are converted strictly to UTF-8 and raw paths opened through `CreateFileW(CREATE_NEW)`. Raw output is binary, existing paths are never truncated, and final file size is read as 64 bits. A file created during an unsuccessful attempt is retained. Windows accepts the old option names but rejects encoded positional files, `--encode`, `--snapshot`, `--preview`, and `--split` before USB initialization; conversion belongs to the frontend after capture. Other legacy options remain parsed. `--probe` negotiates a stream; it is not a read-only discovery command.

JSON consists of individual newline-terminated objects. Frontends should parse lines beginning with `{`, retain human diagnostic lines, and continuously drain both stdout and stderr. Discovery returns `type: devices`, `devices`, `count`, and `capture_candidates`; each device exposes numeric `vid`, `pid`, `bus`, `address`, `vs_iface`, `dv_format_index`, `vc_iface`, `mtt_id`, boolean `dv`, and string `name`. Standalone `status-json` uses GET requests and reports `type: tape`, `state_result`, `state` (number or null), `timecode_result`, `tc` (string or null), and `physical_end_verified: false`. A single status query cannot prove physical tape end. Standalone JSON transport commands report `type: transport_set`, `action`, `mode`, and `set_result` even on failure. Successful one-byte SET returns exactly 1; a short result is a failure.

After the iso transfer ring is submitted, capture emits `type: ready`. Only then does `--play-after-stream` send one playback SET and emit `type: play` with `set_result`. The legacy `--play` is an alias for that timing. There is no automatic reset or command retry. A rejected playback SET terminates the session with a nonzero result while retaining any raw data already accepted.

With `--stop-on-stdin`, the GUI must keep a redirected input pipe open. Send ASCII `stop\n` and wait for process completion. EOF also requests shutdown, including parent exit. The control thread only sets atomics; the USB event thread cancels its own transfers, retires callback slots, and then drains the raw writer. Avoid killing the process while it finalizes. Windows console stdin is intentionally not consumed by this control thread; use a pipe or Ctrl+C. Pending cancellations are never freed while callbacks still refer to them: if cancellation cannot finish within the bounded grace period, resources remain alive until process exit and the run fails after the raw writer drains.

Transport polling happens about every five seconds. Automatic `camera_stopped` completion requires playback actually observed in this run, captured bytes, then successful Stop64 reads continuously spanning the requested interval. A failed read resets that interval. This completion **does not verify physical tape end**. `physical_end_verified` becomes true only after two consecutive successful 0x75 reads, at least ten seconds into the stream, with captured data. No duration estimate, frozen timecode, idle timeout, or ordinary Stop64 promotes that flag.

Final `type: done` contains `bytes`, `written`, `file_bytes`, `drop`, `uvc_err`, `hdrbad`, `ovf`, `enc_bytes`, `write_error` (legacy integer), `stop_reason`, `physical_end_verified`, `device_lost`, `transport_failed`, `play_attempted`, and `play_set_result` (integer or null). `bytes` is total accepted DV payload; overflow is reported independently and means loss. For this backend, require process result 0, a received done event, `write_error == 0`, no lost/failed transport, and `written == file_bytes` for file finalization. A separate zero-transport-loss claim requires every loss counter explicitly known and zero. Partial captures can be retained even when that gate fails. Missing counters are unknown, never zero. A valid file does not on its own establish whole-tape coverage. Raw may end with a partial DV frame, which must be retained and disclosed by later conversion.

Reasons: `stdin_stop`, `parent_stdin_closed`, `signal`, `physical_end`, `camera_stopped`, `idle_timeout`, `max_seconds`, `device_lost`, `transport_failed`, `write_error`, `stdin_error`, `play_failed`, or `completed`. `--stop` performs one best-effort tape stop SET after iso callbacks retire; it does not change the capture's completion scope.

## Offline verification

Configure against a local libusb 1.0.30 build on macOS, build normally, then run `ctest --test-dir build/native --output-on-failure`. Tests create only synthetic temporary files and libusb transfer objects; they never create a libusb context, enumerate devices, or send USB requests. Callback tests cover NO_DEVICE, cancellation, stop, failed requeue, owner-slot clearing, and draining already accepted bytes. Control tests distinguish ordinary Stop64 from physical end and verify stop-line/EOF shutdown. Platform tests cover UTF-8 filenames, binary bytes, exclusive creation, a synthetic size above 4 GiB, bounded pipe reads, EOF, and POSIX child wait. Windows test sources also compile with LLVM-MinGW; execution on Windows remains a separate acceptance step.
