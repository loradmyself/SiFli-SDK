@echo off
echo ========================================
echo ChaCha20 Performance Optimization Build
echo ========================================

echo.
echo [1/3] Cleaning previous build...
if exist "project\build_sf32lb52-lchspi-ulp_hcpu" (
    rmdir /s /q "project\build_sf32lb52-lchspi-ulp_hcpu"
)

echo.
echo [2/3] Building project...
cd project
call ..\..\..\..\export.ps1
scons --board=sf32lb52-lchspi-ulp_hcpu -j8
if %errorlevel% neq 0 (
    echo Build failed!
    exit /b 1
)

echo.
echo [3/3] Build successful!
echo.
echo ========================================
echo Testing Instructions:
echo ========================================
echo 1. Flash the firmware to SF32LB52 board
echo 2. Open serial console (115200 baud)
echo 3. Run the following commands:
echo.
echo    # Check cache status
echo    chacha20_oberon_test 6
echo.
echo    # Run optimized throughput test
echo    chacha20_oberon_test 2
echo.
echo    # Run optimized latency test
echo    chacha20_oberon_test 3
echo.
echo    # Compare MbedTLS vs Oberon (if both compiled)
echo    chacha20_oberon_test 5
echo.
echo    # Run all tests
echo    chacha20_oberon_test 4
echo ========================================
echo.
echo Target: 15 MB/s
echo Current baseline: 7 MB/s
echo Expected improvement: 50-80%%
echo.
pause
