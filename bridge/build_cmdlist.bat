@echo off
REM cmdlist_probe -- observe-only hook on CommandList::Add. Separate DLL so it can
REM coexist with the defer hook; %1 suffix because an injected DLL stays locked.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_cmd.obj                  || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\cmdlist_probe.cpp /Fo:out\cmdlist_probe.obj    || exit /b 1
ml64 /nologo /c /Fo out\applyrelay_cmd.obj src\applyrelay_probe.asm                || exit /b 1
link /nologo /DLL /OUT:out\tpf2_cmdlist%1.dll out\hook_cmd.obj out\cmdlist_probe.obj out\applyrelay_cmd.obj || exit /b 1
echo BUILD CMDLIST OK
