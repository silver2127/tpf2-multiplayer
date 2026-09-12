<#
.SYNOPSIS
Build and run tools\datadir_nonascii_test.cpp: Tpf2mpPublishDataDir (native\src\datadir.h)
for a Windows profile folder with non-ASCII characters.

  tools\datadir_nonascii_test.ps1

Builds with /MD so the test's getenv is the UCRT getenv the game's Lua os.getenv calls.
Everything is created under %TEMP%\tpf2mp_datadir_test; nothing else is touched.
#>
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$work = Join-Path $env:TEMP "tpf2mp_datadir_test"
New-Item -ItemType Directory -Force $work | Out-Null
$src = Join-Path $PSScriptRoot "datadir_nonascii_test.cpp"
$exe = Join-Path $work "datadir_nonascii_test.exe"
$cmd = "`"$vcvars`" >nul && cl /nologo /utf-8 /EHsc /MD /W3 `"$src`" /Fe:`"$exe`" /Fo:`"$work\\`" >`"$work\build.log`" 2>&1"
cmd.exe /c $cmd
if (-not (Test-Path $exe)) { Get-Content "$work\build.log"; throw "test build failed" }
& $exe $work
exit $LASTEXITCODE
