@echo off
REM probe_apply — standalone applyProposal observation DLL (see src/probe_apply.cpp).
REM Links NONE of the live bridge's networking on purpose: it can only watch.
REM Inject with injector.exe into a running instance.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_probe.obj                || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\probe_apply.cpp /Fo:out\probe_apply.obj        || exit /b 1
REM applyrelay_probe.asm, NOT applyrelay.asm: the probe variant also hands the
REM game's return address to the handler. Kept as a separate file so the live
REM bridge's relay (shared with capture.cpp) stays byte-identical.
ml64 /nologo /c /Fo out\applyrelay_probe.obj src\applyrelay_probe.asm             || exit /b 1
REM Optional %1 suffix. Once injected, a DLL stays locked for the life of the
REM process, so rebuilding to the same name fails with LNK1104 while the game is
REM running -- and restarting the game to relink would throw away the very state
REM being investigated. Build to a new name instead: probe_apply 2, 3, ...
link /nologo /DLL /OUT:out\tpf2_probe_apply%1.dll out\hook_probe.obj out\probe_apply.obj out\applyrelay_probe.obj || exit /b 1

echo BUILD PROBE OK
