@echo off
:: ============================================================
:: run_tests.bat  —  Build and run HiveMind network unit tests
::
:: Usage:
::   run_tests.bat          run all tests
::   run_tests.bat build    build only, don't run
:: ============================================================

set "PROJECT=%~dp0"
if "%PROJECT:~-1%"=="\" set "PROJECT=%PROJECT:~0,-1%"
set "BUILD_DIR=%PROJECT%\build"
set "TEST_EXE=%BUILD_DIR%\test_network.exe"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [error] Project not built yet. Run build.bat first.
    pause & exit /b 1
)

if not exist "%PROJECT%\detect_toolchain.ps1" (
    echo [error] detect_toolchain.ps1 not found.
    pause & exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT%\detect_toolchain.ps1"
if %errorlevel% neq 0 ( echo [error] Toolchain detection failed. & pause & exit /b 1 )

for /f "usebackq tokens=1,* delims==" %%A in ("%TEMP%\hm_detect.txt") do set "%%A=%%B"
del "%TEMP%\hm_detect.txt" >nul 2>&1

set "PATH=%NINJA_DIR%;%MSVC_BIN%;%PATH%"
set "INCLUDE=%MSVC_INC%;%WINSDK_INC%\ucrt;%WINSDK_INC%\um;%WINSDK_INC%\shared"
set "LIB=%MSVC_LIB%;%WINSDK_LIB%\ucrt\x64;%WINSDK_LIB%\um\x64"

echo.
echo [test] Building tests...
"%CM%" --build "%BUILD_DIR%" --target test_network test_hook -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 (
    echo [error] Build failed.
    pause & exit /b 1
)

if /i "%1"=="build" (
    echo [test] Build complete: %TEST_EXE%
    exit /b 0
)

echo.
echo [test] Running network tests...
echo.
"%TEST_EXE%"
set "NET_EXIT=%errorlevel%"

set "HOOK_EXE=%BUILD_DIR%\test_hook.exe"
echo.
echo [test] Running hook tests...
echo.
"%HOOK_EXE%"
set "HOOK_EXIT=%errorlevel%"

set /a "EXIT_CODE=%NET_EXIT% + %HOOK_EXIT%"

echo.
if %EXIT_CODE% equ 0 (
    echo [test] All tests passed.
) else (
    echo [test] Some tests failed ^(exit code %EXIT_CODE%^).
)
echo.
pause
exit /b %EXIT_CODE%