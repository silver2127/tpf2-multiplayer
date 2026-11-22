<#
.SYNOPSIS
Build and run tools\save_zstd_win_test.cpp: the Windows save stream (native\src\save_zstd.h
over the built-in zstd, native\third_party\zstd) in boost's call pattern, round-tripped,
and 1 worker against 4 timed.

  tools\save_zstd_win_test.ps1

Compiles zstd the way native\build.bat's slice target does (ZSTD_MULTITHREAD, no asm).
Everything is created under %TEMP%\tpf2mp_save_zstd_test; nothing else is touched.
#>
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$env = Join-Path $PSScriptRoot "msvc_env.bat"
$work = Join-Path $env:TEMP "tpf2mp_save_zstd_test"
New-Item -ItemType Directory -Force $work | Out-Null
$src = Join-Path $PSScriptRoot "save_zstd_win_test.cpp"
$z = Join-Path $repo "native\third_party\zstd\lib"
$exe = Join-Path $work "save_zstd_win_test.exe"
$bat = Join-Path $work "build.bat"
@(
    '@echo off',
    "call `"$env`" >nul 2>nul || exit /b 1",
    "cd /d `"$work`"",
    "cl /nologo /O2 /MT /W0 /c /DZSTD_MULTITHREAD /DZSTD_DISABLE_ASM /DZSTD_LEGACY_SUPPORT=0 `"$z\common\*.c`" `"$z\compress\*.c`" `"$z\decompress\*.c`" > `"$work\build.log`" 2>&1 || exit /b 1",
    "cl /nologo /O2 /MT /EHsc /W3 `"$src`" *.obj /Fe:`"$exe`" >> `"$work\build.log`" 2>&1"
) | Set-Content -Path $bat -Encoding ASCII
Remove-Item "$work\*.obj" -ErrorAction SilentlyContinue
if (Test-Path $exe) { Remove-Item $exe }
cmd.exe /c "`"$bat`""
if (-not (Test-Path $exe)) { if (Test-Path "$work\build.log") { Get-Content "$work\build.log" | Select-Object -Last 30 }; throw "test build failed" }
& $exe
exit $LASTEXITCODE
