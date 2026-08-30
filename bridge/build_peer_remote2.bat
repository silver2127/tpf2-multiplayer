@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc /c peer_test\peer_remote2.cpp /Fo:out\peer_remote2.obj || exit /b 1
link /nologo /OUT:out\peer_remote2.exe out\peer_remote2.obj out\net.obj             || exit /b 1
echo BUILD PEER REMOTE2 OK
