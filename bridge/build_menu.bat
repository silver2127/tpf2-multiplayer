@echo off
REM menu_hook -- native Multiplayer button in the TF2 title menu.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /EHsc /utf-8 /c src\hook.cpp /Fo:out\hook_menu.obj        || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /utf-8 /c src\menu_hook.cpp /Fo:out\menu_hook.obj   || exit /b 1
link /nologo /DLL /OUT:out\tpf2_menu%1.dll out\hook_menu.obj out\menu_hook.obj user32.lib gdi32.lib advapi32.lib || exit /b 1
echo BUILD MENU OK
REM auto-deploy to the workshop out dir the proxy loads from (non-fatal: skipped
REM silently if the game is running and holds the dll open).
set "DEST=C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\3710243057\recon\m4\out\tpf2_menu.dll"
copy /y "out\tpf2_menu%1.dll" "%DEST%" >nul 2>&1 && (echo DEPLOYED to workshop out) || (echo DEPLOY SKIPPED ^(dll locked by running game -- redeploy after relaunch^))
