@echo off
setlocal EnableExtensions
REM tpf2_bigmap.dll -- a tpf2mp plugin. Its one dependency on the rest of the
REM repo is the plugin ABI, native\src\plugin\tpf2mp_plugin.h.
REM native\build.bat bigmap (and all) call this script.
REM
REM Optional %1 suffix: a loaded dll stays locked for the life of the process,
REM so rebuilding to the same name fails with LNK1104 while the game is running.
REM
REM   build.bat            -> out\tpf2_bigmap.dll
REM   build.bat -deploy    -> also copy into the host's user plugin dir,
REM                           %LOCALAPPDATA%\tpf2mp\data\plugins\ (the datadir.h
REM                           contract -- the same directory every other runtime
REM                           file lives in, so there is one path source, not two)
REM
REM NOTE: this script does NOT call vcvars64.bat. VsDevCmd's ext\vcvars.bat step
REM has been seen failing on some VS 2022 17.x installs with a cmd parse error
REM ("... was unexpected at this time") that takes down the whole developer
REM prompt. cl.exe only needs INCLUDE/LIB/PATH, so we locate the MSVC toolset
REM and the Windows SDK directly (vswhere + directory scan) instead.

REM ---- locate the VS install: vswhere first, stock Build Tools path second ----
set "VSROOT="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
)
if not defined VSROOT (
    echo MSVC not found -- install VS 2022 Build Tools with the "Desktop development with C++" workload
    exit /b 1
)

REM ---- newest MSVC toolset under <VSROOT>\VC\Tools\MSVC ----
set "MSVCVER="
for /f "usebackq delims=" %%i in (`dir /b /ad-h /o-n "%VSROOT%\VC\Tools\MSVC"`) do (
    set "MSVCVER=%%i"
    goto :msvc_found
)
:msvc_found
if not defined MSVCVER (
    echo no MSVC toolset under "%VSROOT%\VC\Tools\MSVC"
    exit /b 1
)
set "VC=%VSROOT%\VC\Tools\MSVC\%MSVCVER%"

REM ---- newest Windows SDK (10.x) that carries both ucrt headers and um\x64 libs ----
set "KITROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "KITVER="
for /f "usebackq delims=" %%i in (`dir /b /ad /o-n "%KITROOT%\Include"`) do (
    if exist "%KITROOT%\Include\%%i\ucrt" if exist "%KITROOT%\Lib\%%i\um\x64" (
        set "KITVER=%%i"
        goto :kit_found
    )
)
:kit_found
if not defined KITVER (
    echo Windows SDK 10 not found under "%KITROOT%"
    exit /b 1
)

set "INCLUDE=%VC%\include;%KITROOT%\Include\%KITVER%\ucrt;%KITROOT%\Include\%KITVER%\um;%KITROOT%\Include\%KITVER%\shared"
set "LIB=%VC%\lib\x64;%KITROOT%\Lib\%KITVER%\ucrt\x64;%KITROOT%\Lib\%KITVER%\um\x64"
set "PATH=%VC%\bin\Hostx64\x64;%PATH%"

cd /d "%~dp0"
if not exist out mkdir out

if /i "%1"=="-pager-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_terrain_pager.cpp /Fo:out\test_terrain_pager.obj /Fe:out\test_terrain_pager.exe || exit /b 1
    exit /b
)
if /i "%1"=="-codec-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_terrain_codec.cpp /Fo:out\test_terrain_codec.obj /Fe:out\test_terrain_codec.exe || exit /b 1
    exit /b
)
if /i "%1"=="-codec-profile" (
    cl /nologo /O2 /MT /W3 /EHsc tools\profile_codec_decode.cpp /Fo:out\profile_codec_decode.obj /Fe:out\profile_codec_decode.exe || exit /b 1
    exit /b
)
if /i "%1"=="-decode-exp" (
    cl /nologo /O2 /MT /W3 /EHsc /FAs /Faout\decode_experiments.asm tools\decode_experiments.cpp /Fo:out\decode_experiments.obj /Fe:out\decode_experiments.exe || exit /b 1
    exit /b
)
if /i "%1"=="-alignbatch-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_alignment_batch.cpp /Fo:out\test_alignment_batch.obj /Fe:out\test_alignment_batch.exe || exit /b 1
    exit /b
)
if /i "%1"=="-serve-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_terrain_serve.cpp /Fo:out\test_terrain_serve.obj /Fe:out\test_terrain_serve.exe || exit /b 1
    exit /b
)
if /i "%1"=="-sidecar-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_terrain_sidecar.cpp /Fo:out\test_terrain_sidecar.obj /Fe:out\test_terrain_sidecar.exe || exit /b 1
    exit /b
)
if /i "%1"=="-smallpager-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_small_pager.cpp /Fo:out\test_small_pager.obj /Fe:out\test_small_pager.exe || exit /b 1
    exit /b
)
if /i "%1"=="-matpager-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_material_pager.cpp /Fo:out\test_material_pager.obj /Fe:out\test_material_pager.exe || exit /b 1
    exit /b
)
if /i "%1"=="-matcodec-test" (
    cl /nologo /O2 /MT /W3 /EHsc tools\test_material_codec.cpp /Fo:out\test_material_codec.obj /Fe:out\test_material_codec.exe || exit /b 1
    exit /b
)
if /i "%1"=="-codec-bench" (
    cl /nologo /O2 /MT /W3 /EHsc tools\benchmark_terrain_codec.cpp /Fo:out\benchmark_terrain_codec.obj /Fe:out\benchmark_terrain_codec.exe || exit /b 1
    exit /b
)

REM The minimap game script travels inside the DLL (src\minimap.h writes it out).
powershell -NoProfile -ExecutionPolicy Bypass -File tools\embed_lua.ps1 -In mod\minimap\bigmap_minimap.lua -Out out\minimap_lua.inc -Name kMinimapLua || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\bigmap.cpp /Fo:out\bigmap.obj || exit /b 1
link /nologo /DLL /MAP:out\tpf2_bigmap.map /OUT:out\tpf2_bigmap.dll out\bigmap.obj          || exit /b 1
echo BUILD BIGMAP OK

REM One install, not two. The plugin host scans the datadir FIRST, so a datadir
REM copy silently shadows a game-folder one -- and the two drift: the datadir
REM gets this build and the repo cfg, while the game folder keeps whatever the
REM user edited. This MUST stay outside the `if` block below: cmd expands a
REM parenthesised block before it runs, so a `call :find_game_dir` inside it
REM expands %GAMEDIR% to nothing and the check passes whatever the answer is.
call :find_game_dir

if /i "%1"=="-deploy" (
    if defined GAMEDIR if exist "%GAMEDIR%\plugins\tpf2_bigmap.dll" (
        echo.
        echo DEPLOY REFUSED: a game-folder install already exists:
        echo     %GAMEDIR%\plugins\tpf2_bigmap.dll
        echo The plugin host scans %%LOCALAPPDATA%%\tpf2mp\data\plugins FIRST, so a
        echo datadir copy would shadow it and run against the repo cfg, not yours.
        echo Remove one of the two installs, or pass -deploy-force to overwrite anyway.
        echo.
        if /i not "%2"=="-deploy-force" exit /b 1
    )
    if not exist "%LOCALAPPDATA%\tpf2mp\data\plugins" mkdir "%LOCALAPPDATA%\tpf2mp\data\plugins"
    copy /y out\tpf2_bigmap.dll "%LOCALAPPDATA%\tpf2mp\data\plugins\" >nul ^
        && echo DEPLOYED to %LOCALAPPDATA%\tpf2mp\data\plugins ^
        || echo DEPLOY SKIPPED ^(dll locked by a running game^)
    REM A datadir deploy must carry the cfg too: the plugin host scans the
    REM datadir plugins folder first, and a plugin there with no cfg next to it
    REM silently runs on built-in defaults (no size ladder, no octree).
    if exist cfg\tpf2_bigmap.cfg copy /y cfg\tpf2_bigmap.cfg "%LOCALAPPDATA%\tpf2mp\data\plugins\" >nul
)
endlocal
exit /b 0

REM ---- locate the game folder, for the -deploy shadow check ----------------
REM A subroutine, not inline: inside a parenthesised block cmd expands the whole
REM block before it runs, so `if not defined GAMEDIR` would keep testing the
REM pre-loop value and the last registry hit would win, not the first.
:find_game_dir
set "GAMEDIR="
if defined TPF2_GAMEDIR if exist "%TPF2_GAMEDIR%\TransportFever2.exe" set "GAMEDIR=%TPF2_GAMEDIR%"
if defined GAMEDIR goto :eof
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\GOG.com\Games" /s /v path 2^>nul ^| findstr /i "path"') do (
    if not defined GAMEDIR if exist "%%b\TransportFever2.exe" set "GAMEDIR=%%b"
)
if defined GAMEDIR goto :eof
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\GOG.com\Games" /s /v path 2^>nul ^| findstr /i "path"') do (
    if not defined GAMEDIR if exist "%%b\TransportFever2.exe" set "GAMEDIR=%%b"
)
if defined GAMEDIR goto :eof
for %%d in (
    "C:\Games\Transport Fever 2"
    "%ProgramFiles(x86)%\Steam\steamapps\common\Transport Fever 2"
    "%ProgramFiles%\Steam\steamapps\common\Transport Fever 2"
    "%ProgramFiles(x86)%\GOG Galaxy\Games\Transport Fever 2"
) do if not defined GAMEDIR if exist "%%~d\TransportFever2.exe" set "GAMEDIR=%%~d"
goto :eof
