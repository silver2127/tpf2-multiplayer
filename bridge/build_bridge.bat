@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /c src\net.cpp /Fo:out\net.obj               || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\bridge_main.cpp /Fo:out\bridge.obj  || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_a.dll out\net.obj out\bridge.obj || exit /b 1
copy /y out\tpf2_bridge_a.dll out\tpf2_bridge_b.dll >nul                || exit /b 1
echo BUILD BRIDGE OK
