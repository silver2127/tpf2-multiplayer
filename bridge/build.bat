@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\hook.cpp /Fo:out\hook.obj         || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\net.cpp /Fo:out\net.obj           || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\capture.cpp /Fo:out\capture.obj   || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\dllmain.cpp /Fo:out\dllmain.obj   || exit /b 1
ml64 /nologo /c src\applyrelay.asm                                    || exit /b 1
move /y applyrelay.obj out\applyrelay.obj >nul                        || exit /b 1
link /nologo /DLL /OUT:out\tpf2_mp.dll out\hook.obj out\net.obj out\capture.obj out\dllmain.obj out\applyrelay.obj || exit /b 1

cl /nologo /O2 /MT /W3 /EHsc /c peer_test\main.cpp /Fo:out\peer_main.obj || exit /b 1
link /nologo /OUT:out\peer_test.exe out\peer_main.obj out\net.obj     || exit /b 1

echo BUILD M4a OK
