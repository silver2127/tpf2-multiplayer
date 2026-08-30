@echo off
REM slice_hook -- capture a player's road, cancel it locally, hand it to lockstep.
REM Reuses deferrelay.asm: its suppress path is generic (restores r10, unwinds the
REM blob's three pushes, returns to the callee's caller), so it works for any
REM hooked function whose return value is discarded at the call site.
REM
REM Optional %1 suffix: an injected DLL stays locked for the life of the process,
REM so rebuilding to the same name fails with LNK1104 while the game is running.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_slice.obj                || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\slice_hook.cpp /Fo:out\slice_hook.obj          || exit /b 1
ml64 /nologo /c /Fo out\deferrelay_slice.obj src\deferrelay_slice.asm                    || exit /b 1
link /nologo /DLL /OUT:out\tpf2_slice%1.dll out\hook_slice.obj out\slice_hook.obj out\deferrelay_slice.obj || exit /b 1

echo BUILD SLICE OK
