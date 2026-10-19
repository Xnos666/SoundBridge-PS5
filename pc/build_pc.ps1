# Builds the PC apps with LLVM (clang) and the Windows SDK:
#   dist\SoundBridge.exe            window app (PS5-style)
#   dist\SoundBridge_Receiver.exe   text version
# Run from the project folder: powershell -File pc\build_pc.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$llvm = "C:\Program Files\LLVM\bin"
$pc = Join-Path $root "pc"
$src = Join-Path $root "src"
$obj = Join-Path $root "build\pc"
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force $obj, $dist | Out-Null

$libs = "-lws2_32", "-lole32", "-lavrt", "-lwinmm", "-liphlpapi"
$warn = "-O2", "-Wall", "-Wextra", "-I$src", "-I$pc"

& "$llvm\llvm-rc.exe" /nologo /fo "$obj\soundbridge.res" "$pc\soundbridge.rc"
if ($LASTEXITCODE) { throw "rc failed" }
& "$llvm\clang.exe" @warn -c "$pc\sb_engine.c" -o "$obj\sb_engine.o"
if ($LASTEXITCODE) { throw "engine failed" }
& "$llvm\clang++.exe" @warn -std=c++17 -c "$pc\gui.cpp" -o "$obj\gui.o"
if ($LASTEXITCODE) { throw "gui failed" }
& "$llvm\clang++.exe" "$obj\gui.o" "$obj\sb_engine.o" "$obj\soundbridge.res" -o "$dist\SoundBridge.exe" `
    -Xlinker /subsystem:windows @libs -ld2d1 -ldwrite -ldwmapi -luser32 -lgdi32
if ($LASTEXITCODE) { throw "link SoundBridge.exe failed" }
& "$llvm\clang.exe" @warn "$pc\console.c" "$obj\sb_engine.o" "$obj\soundbridge.res" -o "$dist\SoundBridge_Receiver.exe" @libs
if ($LASTEXITCODE) { throw "link SoundBridge_Receiver.exe failed" }
Get-Item "$dist\SoundBridge.exe", "$dist\SoundBridge_Receiver.exe" | Select-Object Name, Length
