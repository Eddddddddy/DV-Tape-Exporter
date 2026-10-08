# Third-party software

The project is licensed GPL-3.0-or-later. Preserve the notices below when distributing source or portable packages.

| Component | License and included material | Source |
| --- | --- | --- |
| uvcdv-capture, modified Windows backend | GPL-3.0-or-later; original copyright retained in source, `src/native/COPYING` and `licenses/uvcdv-GPL-3.0.txt` | [Upstream](https://github.com/boris-timofeenko/uvcdv-capture), modified source in `src/native` |
| libusb 1.0.30, dynamically linked | LGPL-2.1-or-later; `licenses/libusb-LGPL-2.1.txt`, `src/native/LIBUSB-COPYING.txt`, corresponding archive `src/third-party/libusb-1.0.30.tar.bz2` | [libusb 1.0.30](https://github.com/libusb/libusb/releases/tag/v1.0.30) |
| Microsoft .NET / Windows Desktop runtime | MIT and component notices; `licenses/dotnet-LICENSE.TXT`, `licenses/dotnet-THIRD-PARTY-NOTICES.TXT`, `licenses/dotnet-WindowsDesktop-LICENSE.txt` | [dotnet/runtime](https://github.com/dotnet/runtime), [dotnet/windowsdesktop](https://github.com/dotnet/windowsdesktop) |
| MinGW runtime / winpthreads | Component notices in `src/native/MINGW-RUNTIME-COPYING.txt`, `src/native/WINPTHREADS-COPYING.txt` | [mingw-w64](https://www.mingw-w64.org/), [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) |

FFmpeg is downloaded separately at the user's request or supplied by the user; it is not included in this repository or the portable release. The application links to the Windows provider listed by [FFmpeg downloads](https://ffmpeg.org/download.html), checks the supplied SHA-256, and retains that build's license and download record. Third-party builds can have their own distribution conditions.
