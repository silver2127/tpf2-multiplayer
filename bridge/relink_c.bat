@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
link /nologo /DLL /OUT:out\tpf2_mp_c.dll out\hook.obj out\net.obj out\capture.obj out\dllmain.obj out\applyrelay.obj || exit /b 1
echo RELINK C OK
