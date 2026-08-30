@echo off
REM Proxy-loader build.
REM   tpf2_bridge_mp.dll -- one bridge for both instances, identity decided at
REM                         runtime from port availability (instance=auto)
REM   alut.dll           -- export-forwarding proxy that loads it before the
REM                         exe entry point, so we exist before the title menu
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

cl /nologo /O2 /MT /W3 /EHsc /c src\net.cpp /Fo:out\net_mp.obj                    || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\savexfer.cpp /Fo:out\savexfer_mp.obj          || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook_mp.obj                  || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\simhook.cpp /Fo:out\simhook_mp.obj            || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\buyhook.cpp /Fo:out\buyhook_mp.obj            || exit /b 1
ml64 /nologo /c /Fo out\simsteprelay_mp.obj src\simsteprelay.asm                  || exit /b 1
ml64 /nologo /c /Fo out\buyrelay_mp.obj src\buyrelay.asm                          || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\bridge_main.cpp /Fo:out\bridge_mp.obj         || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_mp.dll out\net_mp.obj out\savexfer_mp.obj out\hook_mp.obj out\simhook_mp.obj out\simsteprelay_mp.obj out\buyhook_mp.obj out\buyrelay_mp.obj out\bridge_mp.obj || exit /b 1

cl /nologo /O2 /MT /W3 /EHsc /LD src\proxy_alut.cpp /Fe:out\alut.dll /Fo:out\proxy_alut.obj || exit /b 1

echo BUILD PROXY OK
