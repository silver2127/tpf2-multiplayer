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
call "%TPF2_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
where cl >nul 2>&1 || exit /b 1
where ml64 >nul 2>&1 || exit /b 1
exit /b 0
