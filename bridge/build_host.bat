@echo off
REM tpf2_pluginhost.dll -- native plugin loader + shared config/build-guard.
REM Also rebuilds the proxy, because build_host adds the host to what it loads.
REM
REM Optional %1 suffix: an already-loaded dll stays locked for the life of the
REM process, so rebuilding to the same name fails with LNK1104 while the game
REM is running.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp            /Fo:out\hook_host.obj   || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\plugin\cfg.cpp      /Fo:out\cfg_host.obj    || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\plugin\host.cpp     /Fo:out\host.obj        || exit /b 1
link /nologo /DLL /OUT:out\tpf2_pluginhost%1.dll out\hook_host.obj out\cfg_host.obj out\host.obj || exit /b 1

cl /nologo /O2 /MT /W3 /EHsc /LD src\proxy_alut.cpp /Fe:out\alut.dll /Fo:out\proxy_alut.obj || exit /b 1

echo BUILD HOST OK
