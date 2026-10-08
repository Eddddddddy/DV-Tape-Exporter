param([string]$NativeToolchain = "", [string]$LibusbDir = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
dotnet publish "$PSScriptRoot/app/DV-Tape-Exporter.csproj" -c Release -r win-x64 --self-contained true -o "$root/bin"
if ($LASTEXITCODE -ne 0) { throw 'GUI build failed' }
dotnet publish "$PSScriptRoot/directshow/DvDirectShow.csproj" -c Release -r win-x64 --self-contained true -o "$root/bin/directshow"
if ($LASTEXITCODE -ne 0) { throw 'DirectShow build failed' }
if ($NativeToolchain -and $LibusbDir) {
  $cc = Join-Path $NativeToolchain 'bin/x86_64-w64-mingw32-clang.exe'
  $env:PATH = (Join-Path $NativeToolchain 'bin') + ';' + $env:PATH
  cmake -S "$PSScriptRoot/native" -B "$root/build/native" -G Ninja -DCMAKE_SYSTEM_NAME=Windows "-DCMAKE_C_COMPILER=$cc" "-DLIBUSB_INCLUDE_DIR=$LibusbDir/include" "-DLIBUSB_LIBRARY=$LibusbDir/MinGW64/static/libusb-1.0.dll.a" -DBUILD_TESTING=OFF
  if ($LASTEXITCODE -ne 0) { throw 'Native configure failed' }
  cmake --build "$root/build/native"
  if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
  New-Item -ItemType Directory -Force "$root/bin/native" | Out-Null
  Copy-Item "$root/build/native/uvcdv-capture.exe" "$root/bin/native/"
  Copy-Item "$LibusbDir/MinGW64/dll/libusb-1.0.dll" "$root/bin/native/"
}
Copy-Item "$root/docs/使用说明.html" "$root/bin/使用说明.html" -Force
Write-Host 'Windows build finished. Keep runtime DLLs and notices with the executables.'
