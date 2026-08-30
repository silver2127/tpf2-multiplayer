@echo off
REM payload_probe -- observe-only hook on the payload wrapper (0x9dd6a0).
REM %1 suffix: an injected DLL stays locked for the life of the process.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_pl.obj                   || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\payload_probe.cpp /Fo:out\payload_probe.obj    || exit /b 1
ml64 /nologo /c /Fo out\applyrelay_pl.obj src\applyrelay_probe.asm                 || exit /b 1
link /nologo /DLL /OUT:out\tpf2_payload%1.dll out\hook_pl.obj out\payload_probe.obj out\applyrelay_pl.obj || exit /b 1
echo BUILD PAYLOAD OK
