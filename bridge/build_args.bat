@echo off
REM args_probe -- observe-only hook on make_cmd::BuildProposal entry, dumping the
REM arguments the caller supplies. %1 suffix: injected DLLs stay locked.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_ar.obj                || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\args_probe.cpp /Fo:out\args_probe.obj       || exit /b 1
ml64 /nologo /c /Fo out\applyrelay_ar.obj src\applyrelay_probe.asm              || exit /b 1
link /nologo /DLL /OUT:out\tpf2_args%1.dll out\hook_ar.obj out\args_probe.obj out\applyrelay_ar.obj || exit /b 1
echo BUILD ARGS OK
