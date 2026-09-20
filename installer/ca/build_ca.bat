@echo off
REM tpf2ca.dll -- the MSI custom-action DLL (game-folder check, stock alut.dll
REM preserve/restore). x64, static CRT, no dependencies beyond msi.dll.
REM Output: installer\out\tpf2ca.dll. Same toolchain setup as native\build.bat:
REM tools\msvc_env.bat finds the x64 MSVC toolchain through vswhere, so a local
REM Build Tools install and a hosted Visual Studio (GitHub's runners) both work.
call "%~dp0..\..\tools\msvc_env.bat" || exit /b 1
cd /d "%~dp0"
if not exist ..\out mkdir ..\out
cl /nologo /O2 /MT /W3 /EHsc /LD tpf2ca.cpp /Fe:..\out\tpf2ca.dll /Fo:..\out\tpf2ca.obj msi.lib || exit /b 1
echo BUILD CA OK
