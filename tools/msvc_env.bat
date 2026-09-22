@echo off
REM Keep the caller's environment: both local Build Tools and hosted VS editions.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    where cl >nul 2>&1 && where ml64 >nul 2>&1 && exit /b 0
)
set "TPF2_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%TPF2_VSWHERE%" (
    echo Visual Studio Installer / vswhere.exe not found. Install the x64 C++ Build Tools.
    exit /b 1
)
set "TPF2_VSROOT="
for /f "usebackq tokens=*" %%I in (`"%TPF2_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TPF2_VSROOT=%%I"
if not defined TPF2_VSROOT (
    echo No Visual Studio installation with the x64 C++ toolchain found.
    exit /b 1
)

REM NOTE: this does NOT call vcvars64.bat. VsDevCmd's ext\vcvars.bat step has been
REM seen failing on some VS 2022 17.x installs with a cmd parse error
REM ("... was unexpected at this time") that takes the whole developer prompt
REM down. cl.exe/ml64.exe only need INCLUDE/LIB/PATH, so the MSVC toolset and the
REM Windows SDK are located directly (vswhere + directory scan) instead.

set "TPF2_MSVCVER="
for /f "usebackq delims=" %%I in (`dir /b /ad-h /o-n "%TPF2_VSROOT%\VC\Tools\MSVC"`) do (
    set "TPF2_MSVCVER=%%I"
    goto :tpf2_msvc_found
)
:tpf2_msvc_found
if not defined TPF2_MSVCVER (
    echo no MSVC toolset under "%TPF2_VSROOT%\VC\Tools\MSVC"
    exit /b 1
)
set "TPF2_VC=%TPF2_VSROOT%\VC\Tools\MSVC\%TPF2_MSVCVER%"

set "TPF2_KITROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "TPF2_KITVER="
for /f "usebackq delims=" %%I in (`dir /b /ad /o-n "%TPF2_KITROOT%\Include"`) do (
    if exist "%TPF2_KITROOT%\Include\%%I\ucrt" if exist "%TPF2_KITROOT%\Lib\%%I\um\x64" (
        set "TPF2_KITVER=%%I"
        goto :tpf2_kit_found
    )
)
:tpf2_kit_found
if not defined TPF2_KITVER (
    echo Windows SDK 10 not found under "%TPF2_KITROOT%"
    exit /b 1
)

set "INCLUDE=%TPF2_VC%\include;%TPF2_KITROOT%\Include\%TPF2_KITVER%\ucrt;%TPF2_KITROOT%\Include\%TPF2_KITVER%\um;%TPF2_KITROOT%\Include\%TPF2_KITVER%\shared"
set "LIB=%TPF2_VC%\lib\x64;%TPF2_KITROOT%\Lib\%TPF2_KITVER%\ucrt\x64;%TPF2_KITROOT%\Lib\%TPF2_KITVER%\um\x64"
set "PATH=%TPF2_VC%\bin\Hostx64\x64;%PATH%"

where cl >nul 2>&1 || exit /b 1
where ml64 >nul 2>&1 || exit /b 1
exit /b 0
