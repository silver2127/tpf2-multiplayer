@echo off
REM native\build.bat <target> [suffix] -- every native DLL from one script.
REM
REM   slice   tpf2_slice.dll             the command-capture hooks (slice_hook.cpp +
REM                                      deferrelay_slice.asm: its suppress path is
REM                                      generic, so it serves every hooked function
REM                                      whose return value the call site discards)
REM   menu    tpf2_menu.dll              the Vulkan-overlay Multiplayer panel (menu_hook.cpp);
REM                                      also copied to the game dir when nothing holds
REM                                      it open
REM   proxy   alut.dll, tpf2_bridge_mp.dll   the export-forwarding proxy that loads us
REM                                      before the exe entry point, and the bridge
REM                                      (identity, relay socket, fractional speed)
REM   host    tpf2_pluginhost.dll        the plugin host shared with TpF2 Big Maps;
REM                                      rebuilds alut.dll too (the host adds to what
REM                                      the proxy loads)
REM   all     proxy, host, menu, slice -- in that order
REM
REM Optional suffix (slice / menu / host): a DLL an instance has loaded stays locked
REM for the life of that process, so relinking to the same name fails with LNK1104
REM while the game runs. "build.bat slice 2" links out\tpf2_slice2.dll instead.
REM Only the plain name ships: tools\deploy_shipping.ps1 refuses a suffixed one.
REM
REM Paths: Build Tools 2022 at the default location (vcvars64.bat below); edit it
REM for another edition. Outputs go to native\out.
setlocal
set "T=%~1"
set "SFX=%~2"
if /i "%T%"=="slice" goto run
if /i "%T%"=="menu"  goto run
if /i "%T%"=="proxy" goto run
if /i "%T%"=="host"  goto run
if /i "%T%"=="all"   goto run
echo usage: build.bat slice^|menu^|proxy^|host^|all [suffix]
exit /b 2

:run
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
set "CC=cl /nologo /O2 /MT /W3 /EHsc"
if /i "%T%"=="all" (
    call :proxy || exit /b 1
    call :host  || exit /b 1
    call :menu  || exit /b 1
    call :slice || exit /b 1
    echo BUILD ALL OK
    exit /b 0
)
call :%T% || exit /b 1
echo BUILD %T% OK
exit /b 0

:slice
%CC% /c src\hook.cpp /Fo:out\hook_slice.obj                                          || exit /b 1
%CC% /c src\slice_hook.cpp /Fo:out\slice_hook.obj                                    || exit /b 1
ml64 /nologo /c /Fo out\deferrelay_slice.obj src\deferrelay_slice.asm                || exit /b 1
link /nologo /DLL /OUT:out\tpf2_slice%SFX%.dll out\hook_slice.obj out\slice_hook.obj out\deferrelay_slice.obj || exit /b 1
exit /b 0

:menu
%CC% /utf-8 /c src\hook.cpp /Fo:out\hook_menu.obj                                    || exit /b 1
%CC% /utf-8 /c src\menu_hook.cpp /Fo:out\menu_hook.obj                               || exit /b 1
ml64 /nologo /c /Fo out\gameuirelay_menu.obj src\gameuirelay.asm                     || exit /b 1
link /nologo /DLL /OUT:out\tpf2_menu%SFX%.dll out\hook_menu.obj out\menu_hook.obj out\gameuirelay_menu.obj user32.lib gdi32.lib advapi32.lib || exit /b 1
REM Deploy to where the proxy loads it from. Non-fatal: a running game holds the
REM dll open, and the copy is simply skipped -- redeploy after the relaunch.
set "GAMEDEST=C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\tpf2_menu.dll"
copy /y "out\tpf2_menu%SFX%.dll" "%GAMEDEST%" >nul 2>&1 && (echo deployed to the game dir) || (echo game-dir deploy skipped: dll locked by a running game -- close it and rerun build.bat menu)
exit /b 0

:proxy
%CC% /c src\net.cpp /Fo:out\net_mp.obj                                               || exit /b 1
%CC% /c src\hook.cpp /Fo:out\hook_mp.obj                                             || exit /b 1
%CC% /c src\speedhook.cpp /Fo:out\speedhook_mp.obj                                   || exit /b 1
ml64 /nologo /c /Fo out\cgamesteprelay_mp.obj src\cgamesteprelay.asm                 || exit /b 1
%CC% /c src\bridge_main.cpp /Fo:out\bridge_mp.obj                                    || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_mp.dll out\net_mp.obj out\hook_mp.obj out\speedhook_mp.obj out\cgamesteprelay_mp.obj out\bridge_mp.obj || exit /b 1
%CC% /LD src\proxy_alut.cpp /Fe:out\alut.dll /Fo:out\proxy_alut.obj                  || exit /b 1
exit /b 0

:host
%CC% /c src\hook.cpp            /Fo:out\hook_host.obj                                || exit /b 1
%CC% /c src\plugin\cfg.cpp      /Fo:out\cfg_host.obj                                 || exit /b 1
%CC% /c src\plugin\host.cpp     /Fo:out\host.obj                                     || exit /b 1
link /nologo /DLL /OUT:out\tpf2_pluginhost%SFX%.dll out\hook_host.obj out\cfg_host.obj out\host.obj || exit /b 1
%CC% /LD src\proxy_alut.cpp /Fe:out\alut.dll /Fo:out\proxy_alut.obj                  || exit /b 1
exit /b 0
