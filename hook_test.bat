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

:: ── Force-copy new hook.dll to stable path ───────────────────
:: The old dll may still be mapped into running processes even
:: after uninstall (registry cleared but already-loaded images
:: stay until those processes exit).  Strategy:
::   1. Wait up to ~3s for processes to drain naturally.
::   2. If still locked, use handle64 or taskkill on known holders.
::   3. If still locked after that, rename the old file out of the
::      way (rename always works even on a mapped dll) and copy
::      the new one in under the original name.

echo.
echo [deploy] Copying new hook.dll to %TOOLS%...

:: First attempt
copy /Y "%BUILD%\hook.dll" "%TOOLS%\hook.dll" >nul 2>&1
if %errorlevel% equ 0 goto :copy_ok

echo [warn] hook.dll locked -- waiting 3s for processes to release it...
ping -n 4 127.0.0.1 >nul

:: Second attempt after wait
copy /Y "%BUILD%\hook.dll" "%TOOLS%\hook.dll" >nul 2>&1
if %errorlevel% equ 0 goto :copy_ok

echo [warn] Still locked -- attempting rename workaround...

:: Rename the locked file out of the way. Windows allows renaming
:: a file that is mapped into memory; it just can't be deleted or
:: overwritten while mapped.
if exist "%TOOLS%\hook.dll.old" del /F "%TOOLS%\hook.dll.old" >nul 2>&1
rename "%TOOLS%\hook.dll" "hook.dll.old" >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Cannot rename old hook.dll -- it may be held by a
    echo         system process.  Close all non-essential apps, then
    echo         re-run this script.
    pause & exit /b 1
)

:: Now copy the new file into the vacated name
copy /Y "%BUILD%\hook.dll" "%TOOLS%\hook.dll" >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Copy failed even after rename. Check disk space and
    echo         permissions on %TOOLS%.
    pause & exit /b 1
)

:: The .old file will be cleaned up next run or on reboot
echo [warn] Old hook.dll.old left in %TOOLS% -- safe to delete after reboot.

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
