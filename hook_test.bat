@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: deploy_and_run.bat
::
:: BEFORE RUNNING — set VCVARS to your vcvars64.bat path below.
:: Run from an ELEVATED command prompt (Run as administrator).
::
:: To uninstall when done: cmake-build-debug\install_hook.exe uninstall
:: ============================================================

:: ── SET THIS ─────────────────────────────────────────────────
:: Syntax must be exactly:  set "VCVARS=<full path>"
:: Example paths:
::   set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
::   set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: ─────────────────────────────────────────────────────────────

set "PROJECT=%~dp0"
if "%PROJECT:~-1%"=="\" set "PROJECT=%PROJECT:~0,-1%"

set "BUILD=%PROJECT%\cmake-build-debug"
set "CMAKE_EXE=C:\Program Files\JetBrains\CLion 2025.2.1\bin\cmake\win\x64\bin\cmake.exe"
set "TOOLS=C:\Tools"
set "LOG=C:\Temp\hook.log"

echo.
echo [deploy] Project root : %PROJECT%
echo [deploy] Build dir    : %BUILD%
echo.

:: ── Check Administrator ──────────────────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Must be run as Administrator.
    pause & exit /b 1
)
echo [ok] Running as Administrator.

:: ── Validate VCVARS ──────────────────────────────────────────
if not defined VCVARS (
    echo [ERROR] VCVARS is not set. Edit this script and set it.
    pause & exit /b 1
)

if not exist "!VCVARS!" (
    echo [ERROR] vcvars64.bat not found at:
    echo         !VCVARS!
    echo         Check the path is correct and the file exists.
    pause & exit /b 1
)

echo [ok] MSVC toolchain : !VCVARS!

:: ── Activate MSVC environment ────────────────────────────────
call "!VCVARS!" >nul 2>&1
echo [ok] MSVC environment activated.

:: ── Create required directories ──────────────────────────────
if not exist "%TOOLS%"  mkdir "%TOOLS%"
if not exist "C:\Temp"  mkdir "C:\Temp"
echo [ok] Directories ready.

:: ── CMake configure ──────────────────────────────────────────
echo.
echo [build] Configuring...
"%CMAKE_EXE%" -S "%PROJECT%" -B "%BUILD%" -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl
if %errorlevel% neq 0 ( echo [ERROR] Configure failed. & pause & exit /b 1 )
echo [ok] Configure done.

:: ── Build ────────────────────────────────────────────────────
echo.
echo [build] Building...
"%CMAKE_EXE%" --build "%BUILD%" --target hook offload_hook install_hook test_runner -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 ( echo [ERROR] Build failed. & pause & exit /b 1 )
echo [ok] Build complete.

:: ── Copy hook.dll to stable path ─────────────────────────────
copy /Y "%BUILD%\hook.dll" "%TOOLS%\hook.dll" >nul
echo [ok] hook.dll at %TOOLS%\hook.dll

:: ── Install AppInit_DLLs ─────────────────────────────────────
echo.
echo [install] Registering hook.dll system-wide...
"%BUILD%\install_hook.exe" install "%TOOLS%\hook.dll"
if %errorlevel% neq 0 ( echo [ERROR] install_hook failed. & pause & exit /b 1 )

echo.
echo [install] Registry state:
"%BUILD%\install_hook.exe" query

:: ── Run test ─────────────────────────────────────────────────
echo.
echo [test] Running test_runner.exe...
"%BUILD%\test_runner.exe"

:: ── Show log tail ─────────────────────────────────────────────
echo.
echo [log] Last 40 lines of %LOG%:
echo ================================================================
if exist "%LOG%" (
    powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 40"
) else (
    echo [WARN] Log not found. hook.dll may not have loaded.
)
echo ================================================================

echo.
echo [done] hook.dll is now system-wide.
echo        Launch any desktop .exe and check %LOG%
echo.
echo        To uninstall: %BUILD%\install_hook.exe uninstall
echo.
pause
