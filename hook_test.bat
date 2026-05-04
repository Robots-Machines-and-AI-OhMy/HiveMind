@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: hook_test.bat
::
:: BEFORE RUNNING — set VCVARS to your vcvars64.bat path below.
:: Run from an ELEVATED command prompt (Run as administrator).
::
:: To uninstall when done: cmake-build-debug\install_hook.exe uninstall
:: ============================================================

:: ── SET THIS ─────────────────────────────────────────────────
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

:: ── Uninstall old hook BEFORE building ───────────────────────
:: Clears AppInit_DLLs so cmake/ninja launch without the stale
:: dll during compilation. Re-registered after the fresh copy.
echo.
echo [install] Unregistering old hook.dll before rebuild...
if exist "%BUILD%\install_hook.exe" (
    "%BUILD%\install_hook.exe" uninstall >nul 2>&1
    echo [ok] Old hook unregistered.
) else (
    echo [warn] install_hook.exe not found yet -- skipping uninstall.
)

:: ── Wipe stale build cache ───────────────────────────────────
:: Required when CMakeLists.txt changes (e.g. new add_dependencies
:: edges). Ninja caches the dependency graph; without a clean wipe
:: it will not see the updated edges and skips building hook.dll /
:: offload_hook.dll before the executables link.
echo.
echo [build] Cleaning build directory...
if exist "%BUILD%" (
    rmdir /S /Q "%BUILD%"
    echo [ok] Build directory wiped.
) else (
    echo [ok] Build directory did not exist -- skipping clean.
)

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

:: ── Copy new hook.dll to stable path (retry loop) ────────────
:: The old dll may still be mapped into processes that loaded it
:: via AppInit_DLLs before the registry key was cleared. Clearing
:: the key stops new loads but does not unmap already-loaded images.
:: We simply retry every 2s until the copy succeeds -- as processes
:: that hold the DLL exit naturally it will eventually release.
:: There is no timeout: press Ctrl-C to abort if needed.

echo.
echo [deploy] Copying new hook.dll to %TOOLS%...
echo [deploy] Retrying every 2s until all holders release the file.
echo [deploy] Press Ctrl-C to abort.

:copy_retry
copy /Y "%BUILD%\hook.dll" "%TOOLS%\hook.dll" >nul 2>&1
if %errorlevel% equ 0 goto :copy_ok
echo [wait]  hook.dll still locked -- retrying in 2s...
ping -n 3 127.0.0.1 >nul
goto :copy_retry

:copy_ok
echo [ok] hook.dll updated at %TOOLS%\hook.dll

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