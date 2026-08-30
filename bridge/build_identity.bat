@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /c src\bridge_main.cpp /Fo:out\bridge5.obj      || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_a5.dll out\net.obj out\bridge5.obj   || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DHARDCODE_B /c src\bridge_main.cpp /Fo:out\bridge_b5H.obj || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_b5H.dll out\net.obj out\bridge_b5H.obj || exit /b 1
echo BUILD IDENTITY BRIDGES OK
