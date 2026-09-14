@echo off
setlocal EnableExtensions
REM Build-only offline Workshop registration prototype; never deploys.
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

cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /LD prototype.cpp /Fo:out\prototype.obj /link /OUT:out\tpf2_workshop_register.dll
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc test.cpp /Fo:out\test.obj /Fe:out\test.exe
if errorlevel 1 exit /b 1
out\test.exe
