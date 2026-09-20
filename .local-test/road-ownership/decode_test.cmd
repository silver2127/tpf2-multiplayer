@echo off
call "C:\Users\james\tpf2-dev\tools\msvc_env.bat" >nul 2>nul || exit /b 1
cl /nologo /EHsc /W4 "C:\Users\james\tpf2-dev\.local-test\road-ownership\decode_test.cpp" /Fo:"C:\Users\james\tpf2-dev\.local-test\road-ownership\decode_test.obj" /Fe:"C:\Users\james\tpf2-dev\.local-test\road-ownership\decode_test.exe" || exit /b 1
"C:\Users\james\tpf2-dev\.local-test\road-ownership\decode_test.exe"
