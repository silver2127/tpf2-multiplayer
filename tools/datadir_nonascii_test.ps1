<#
.SYNOPSIS
Build and run tools\datadir_nonascii_test.cpp: Tpf2mpPublishDataDir (native\src\datadir.h)
for a Windows profile folder with non-ASCII characters, and for relative or Unix-style
TPF2MP_DATADIR pins (the Proton case).

  tools\datadir_nonascii_test.ps1

Builds with /MD so the test's getenv is the UCRT getenv the game's Lua os.getenv calls.
The toolchain comes from tools\msvc_env.bat (Build Tools or a hosted edition), called
from a small batch file: cmd /c strips the outer quotes of a command that starts with
a quoted path, which broke the earlier one-line form. Everything is created under
%TEMP%\tpf2mp_datadir_test; nothing else is touched.
#>
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$env = Join-Path $PSScriptRoot "msvc_env.bat"
$work = Join-Path $env:TEMP "tpf2mp_datadir_test"
New-Item -ItemType Directory -Force $work | Out-Null
$src = Join-Path $PSScriptRoot "datadir_nonascii_test.cpp"
$exe = Join-Path $work "datadir_nonascii_test.exe"
$bat = Join-Path $work "build.bat"
@(
    '@echo off',
    "call `"$env`" >nul 2>nul || exit /b 1",
    "cl /nologo /utf-8 /EHsc /MD /W3 `"$src`" /Fe:`"$exe`" /Fo:`"$work\\`" > `"$work\build.log`" 2>&1"
) | Set-Content -Path $bat -Encoding ASCII
if (Test-Path $exe) { Remove-Item $exe }
cmd.exe /c "`"$bat`""
if (-not (Test-Path $exe)) { if (Test-Path "$work\build.log") { Get-Content "$work\build.log" }; throw "test build failed" }
& $exe $work
exit $LASTEXITCODE
