@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /c src\bridge_main.cpp /Fo:out\bridge3.obj   || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_a3.dll out\net.obj out\bridge3.obj || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DHARDCODE_B /c src\bridge_main.cpp /Fo:out\bridge_b3H.obj || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_b3H.dll out\net.obj out\bridge_b3H.obj || exit /b 1
echo BUILD TUNED BRIDGES OK
