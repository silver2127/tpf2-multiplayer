@echo off
REM tools\test_tplz.bat -- build and run the terrain wire codec test (tools\test_tplz.cpp)
REM as a checked debug build (the CRT debug heap catches a write past a buffer).
setlocal
call "%~dp0msvc_env.bat" || exit /b 1
set "OUT=%~dp0..\native\out\test_tplz"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /Od /MTd /RTC1 /EHsc /std:c++17 "%~dp0test_tplz.cpp" /Fe:"%OUT%\test_tplz.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\\" || exit /b 1
"%OUT%\test_tplz.exe"
