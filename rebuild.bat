@echo off
setlocal
echo =======================================================
echo   1. Building native C++ binaries...
echo =======================================================
pushd native
call build.bat all
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Native build failed!
    popd
    exit /b 1
)
popd

echo =======================================================
echo   2. Deploying to Transport Fever 2...
echo =======================================================
powershell -ExecutionPolicy Bypass -File tools\deploy_shipping.ps1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Deployment failed!
    exit /b 1
)

echo =======================================================
echo   3. Validating network and version tests...
echo =======================================================
python tools\version_gate_test.py
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Version gate test failed!
    exit /b 1
)
python tools\test_rto_resilience.py
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Resilience test failed!
    exit /b 1
)

echo =======================================================
echo   SUCCESS: All binaries built, deployed, and verified!
echo =======================================================
