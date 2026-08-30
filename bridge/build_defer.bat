@echo off
REM defer_hook -- the first hook in this project that can CANCEL the function it
REM intercepts. Suppression is off unless tpf2_defer.cfg says suppress=1.
REM
REM Optional %1 suffix: an injected DLL stays locked for the life of the process,
REM so rebuilding to the same name fails with LNK1104 while the game is running.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_defer.obj                || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\defer_hook.cpp /Fo:out\defer_hook.obj          || exit /b 1
ml64 /nologo /c /Fo out\deferrelay.obj src\deferrelay.asm                          || exit /b 1
link /nologo /DLL /OUT:out\tpf2_defer%1.dll out\hook_defer.obj out\defer_hook.obj out\deferrelay.obj || exit /b 1

echo BUILD DEFER OK
