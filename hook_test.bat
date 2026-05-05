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
set "NINJA_EXE=C:\Program Files\JetBrains\CLion 2025.2.1\bin\ninja\win\x64\ninja.exe"
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

:: ── STEP 1: Uninstall BEFORE wiping build dir ────────────────
:: install_hook.exe lives in the build dir. Uninstall must run
:: FIRST — before rmdir — otherwise the exe is gone and the
:: registry keys stay set, leaving the old locked dll registered.
echo.
echo [install] Unregistering old hook.dll...
if exist "%BUILD%\install_hook.exe" (
    "%BUILD%\install_hook.exe" uninstall >nul 2>&1
    echo [ok] Old hook unregistered.
) else (
    echo [warn] install_hook.exe not found -- skipping uninstall.
    echo        If hook.dll is registered it may still be locked.
)

:: ── STEP 2: Wipe build cache AFTER uninstall ─────────────────
echo.
echo [build] Cleaning build directory...
if exist "%BUILD%" rmdir /S /Q "%BUILD%"
:: Delete CMakeCache.txt directly in case rmdir left it behind
:: (happens when CLion or antivirus holds a handle open).
if exist "%BUILD%\CMakeCache.txt" del /F /Q "%BUILD%\CMakeCache.txt"
if exist "%BUILD%\CMakeCache.txt" (
    echo [ERROR] Cannot clear CMakeCache.txt.
    echo         Close CLion completely then re-run this script.
    pause & exit /b 1
)
echo [ok] Build directory cleaned.

:: ── STEP 3: Configure ────────────────────────────────────────
echo.
echo [build] Configuring...
"%CMAKE_EXE%" -S "%PROJECT%" -B "%BUILD%" -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%"
if %errorlevel% neq 0 ( echo [ERROR] Configure failed. & pause & exit /b 1 )
echo [ok] Configure done.

:: ── STEP 4: Build ────────────────────────────────────────────
echo.
echo [build] Building...
"%CMAKE_EXE%" --build "%BUILD%" --target hook offload_hook install_hook test_runner -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 ( echo [ERROR] Build failed. & pause & exit /b 1 )
echo [ok] Build complete.

:: ── STEP 5: Deploy hook.dll and install system-wide ──────────
:: Each run gets a unique timestamped filename so a previously
:: locked copy (still loaded by AppInit processes) never blocks
:: the new deployment. Old files accumulate in C:\Tools but are
:: harmless and can be cleaned up manually after a reboot.
echo.
echo [deploy] Deploying hook.dll to %TOOLS%...

:: Build a unique name: hook_HHMMSS.dll
for /f "tokens=1-3 delims=:." %%a in ("%TIME: =0%") do (
    set "HOOK_TS=%%a%%b%%c"
)
set "HOOK_DEPLOY=%TOOLS%\hook_%HOOK_TS%.dll"

copy /Y "%BUILD%\hook.dll" "%HOOK_DEPLOY%" >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Cannot write to %TOOLS%.
    echo         Check permissions on %TOOLS%.
    pause & exit /b 1
)
echo [ok] Deployed as %HOOK_DEPLOY%

:: ── STEP 6: Install AppInit_DLLs ─────────────────────────────
echo.
echo [install] Registering %HOOK_DEPLOY% system-wide...
"%BUILD%\install_hook.exe" install "%HOOK_DEPLOY%"
if %errorlevel% neq 0 ( echo [ERROR] install_hook failed. & pause & exit /b 1 )



echo.
echo [install] Registry state:
"%BUILD%\install_hook.exe" query

:: ── STEP 7: Run test ─────────────────────────────────────────
echo.
echo [test] Running test_runner.exe...
"%BUILD%\test_runner.exe"

:: ── STEP 8: Show log tail ─────────────────────────────────────
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