@echo off
REM v7 bridges: chunked transport (long lines), LF event writes, working
REM sidecar cfg lookup, truncate-aware tail.
REM One dll per instance -- UDP works both ways, so the old a3+a6 file-relay
REM double-injection is gone.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

cl /nologo /O2 /MT /W3 /EHsc /c src\net.cpp /Fo:out\net7.obj                         || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\savexfer.cpp /Fo:out\savexfer7.obj               || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /c src\bridge_main.cpp /Fo:out\bridge7.obj              || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_a7.dll out\net7.obj out\savexfer7.obj out\bridge7.obj || exit /b 1

cl /nologo /O2 /MT /W3 /EHsc /DHARDCODE_B /c src\bridge_main.cpp /Fo:out\bridge7b.obj || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bridge_b7H.dll out\net7.obj out\savexfer7.obj out\bridge7b.obj || exit /b 1

echo BUILD V7 OK
