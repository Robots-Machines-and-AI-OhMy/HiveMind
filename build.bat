@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: build.bat  —  HiveMind full build script
::
:: Builds everything from a fresh clone in order:
::   1. patch_deps      — patches NuRaft and MSQuic source
::   2. MSQuic          — built with PowerShell 7
::   3. NuRaft          — built with NMake + MSVC
::   4. HiveMind        — built with Ninja + MSVC
::
:: Usage:
::   build.bat            Debug build (default)
::   build.bat Release    Release build
::   build.bat clean      Wipe all three build folders
:: ============================================================

set "BUILD_TYPE=Debug"
if /i "%1"=="Release" set "BUILD_TYPE=Release"

set "PROJECT=%~dp0"
if "%PROJECT:~-1%"=="\" set "PROJECT=%PROJECT:~0,-1%"

set "MSQUIC_DIR=%PROJECT%\msquic"
set "NURAFT_DIR=%PROJECT%\NuRaft"
set "NURAFT_BUILD=%NURAFT_DIR%\build"
set "HiveMind_BUILD=%PROJECT%\build"

:: ── Clean ────────────────────────────────────────────────────
if /i "%1"=="clean" (
    echo [clean] Wiping build folders...
    if exist "%NURAFT_BUILD%"   rmdir /S /Q "%NURAFT_BUILD%"
    if exist "%HiveMind_BUILD%" rmdir /S /Q "%HiveMind_BUILD%"
    echo [clean] Done. MSQuic artifacts are kept ^(run msquic\scripts\build.ps1 to rebuild^).
    pause
    exit /b 0
)

echo.
echo ============================================================
echo  HiveMind Full Build
echo ============================================================
echo.

:: ════════════════════════════════════════════════════════════
:: SECTION 1 — Patch dependencies
:: ════════════════════════════════════════════════════════════
echo [1/4] Patching dependencies...
echo.

if not exist "%PROJECT%\patch_deps.ps1" (
    echo ERROR: patch_deps.ps1 not found in project root.
    pause & exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT%\patch_deps.ps1"
if %errorlevel% neq 0 (
    echo.
    echo ERROR: patch_deps failed. See output above.
    pause & exit /b 1
)
echo.

:: ════════════════════════════════════════════════════════════
:: SECTION 2 — Build MSQuic with PowerShell 7
:: ════════════════════════════════════════════════════════════
echo [2/4] Building MSQuic...
echo.

:: Skip if already built
if exist "%MSQUIC_DIR%\artifacts\bin\windows\x64_Debug_openssl\msquic.lib" (
    echo [skip] MSQuic already built.
    goto :nuraft
)

:: Find PowerShell 7 (pwsh.exe)
set "PWSH="
if not defined PWSH if exist "C:\Program Files\PowerShell\7\pwsh.exe"              set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not defined PWSH if exist "C:\Program Files\PowerShell\7-preview\pwsh.exe"      set "PWSH=C:\Program Files\PowerShell\7-preview\pwsh.exe"
if not defined PWSH where pwsh >nul 2>&1 && set "PWSH=pwsh"

if not defined PWSH (
    echo ERROR: PowerShell 7 ^(pwsh.exe^) not found.
    echo Install from: https://aka.ms/powershell
    echo or run: winget install Microsoft.PowerShell
    pause & exit /b 1
)

echo Using PowerShell 7: %PWSH%
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass ^
    -Command "Set-Location '%MSQUIC_DIR%'; .\scripts\build.ps1 -Tls openssl"

if %errorlevel% neq 0 (
    echo.
    echo ERROR: MSQuic build failed. Cleaning MSQuic artifacts...
    if exist "%MSQUIC_DIR%\artifacts" rmdir /S /Q "%MSQUIC_DIR%\artifacts"
    if exist "%MSQUIC_DIR%\build"     rmdir /S /Q "%MSQUIC_DIR%\build"
    pause & exit /b 1
)
echo.
echo [ok] MSQuic built.
echo.

:: ════════════════════════════════════════════════════════════
:: SECTION 3 — Build NuRaft with NMake
:: ════════════════════════════════════════════════════════════
:nuraft
echo [3/4] Building NuRaft...
echo.

:: Skip if already built
if exist "%NURAFT_BUILD%\nuraft.lib" (
    echo [skip] NuRaft already built.
    goto :HiveMind
)

:: Find vcvars64.bat for the x64 Native Tools environment
set "VCVARS="
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"         set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"      set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"  set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"  set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"          set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"            set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo ERROR: Visual Studio Build Tools not found.
    echo Install from: https://visualstudio.microsoft.com/downloads/
    pause & exit /b 1
)

call "%VCVARS%" >nul 2>&1
echo Compiler ready.

:: Find cmake
set "CMAKE_EXE="
if not defined CMAKE_EXE if exist "C:\Program Files\CMake\bin\cmake.exe"       set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files (x86)\CMake\bin\cmake.exe"
if not defined CMAKE_EXE where cmake >nul 2>&1
if not defined CMAKE_EXE set "CMAKE_EXE=cmake"

if exist "%NURAFT_BUILD%\CMakeCache.txt" rmdir /S /Q "%NURAFT_BUILD%"
mkdir "%NURAFT_BUILD%"

pushd "%NURAFT_BUILD%"

"%CMAKE_EXE%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ..
if %errorlevel% neq 0 (
    popd
    echo ERROR: NuRaft cmake configure failed. Cleaning...
    rmdir /S /Q "%NURAFT_BUILD%"
    pause & exit /b 1
)

nmake
if %errorlevel% neq 0 (
    popd
    echo ERROR: NuRaft nmake failed. Cleaning...
    rmdir /S /Q "%NURAFT_BUILD%"
    pause & exit /b 1
)

popd
echo.
echo [ok] NuRaft built.
echo.

:: ════════════════════════════════════════════════════════════
:: SECTION 4 — Build HiveMind
:: ════════════════════════════════════════════════════════════
:HiveMind
echo [4/4] Building HiveMind (%BUILD_TYPE%)...
echo.

if not exist "%PROJECT%\detect_toolchain.ps1" (
    echo ERROR: detect_toolchain.ps1 not found in project root.
    pause & exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT%\detect_toolchain.ps1"
if %errorlevel% neq 0 (
    echo ERROR: Toolchain detection failed.
    pause & exit /b 1
)

for /f "usebackq tokens=1,* delims==" %%A in ("%TEMP%\hm_detect.txt") do set "%%A=%%B"
del "%TEMP%\hm_detect.txt" >nul 2>&1

if not defined CL_EXE    ( echo ERROR: cl.exe not found.  & pause & exit /b 1 )
if not defined NINJA_DIR ( echo ERROR: ninja not found.   & pause & exit /b 1 )
if not defined CM        ( echo ERROR: cmake not found.   & pause & exit /b 1 )

set "PATH=%NINJA_DIR%;%MSVC_BIN%;%PATH%"
set "INCLUDE=%MSVC_INC%;%WINSDK_INC%\ucrt;%WINSDK_INC%\um;%WINSDK_INC%\shared"
set "LIB=%MSVC_LIB%;%WINSDK_LIB%\ucrt\x64;%WINSDK_LIB%\um\x64"

echo Toolchain:
echo   cl   : %CL_EXE%
echo   mt   : %MT_EXE%
echo   ninja: %NINJA_DIR%
echo   cmake: %CM%
echo.

if exist "%HiveMind_BUILD%\CMakeCache.txt" rmdir /S /Q "%HiveMind_BUILD%"

:: Put the SDK bin dir on PATH so cmake's vs_link_exe wrapper finds rc.exe
for %%F in ("%MT_EXE%") do set "SDK_BIN=%%~dpF"
set "PATH=%SDK_BIN%;%PATH%"

:: Convert backslashes to forward slashes for cmake -D arguments
set "CL_FWD=%CL_EXE:\=/%"
set "LINK_FWD=%LINK_EXE:\=/%"
set "MT_FWD=%MT_EXE:\=/%"
set "RC_FWD=%RC_EXE:\=/%"

"%CM%" -S "%PROJECT%" -B "%HiveMind_BUILD%" ^
    -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_COMPILER="%CL_FWD%" ^
    -DCMAKE_CXX_COMPILER="%CL_FWD%" ^
    -DCMAKE_LINKER="%LINK_FWD%" ^
    -DCMAKE_MT="%MT_FWD%" ^
    -DCMAKE_RC_COMPILER="%RC_FWD%"

if %errorlevel% neq 0 (
    echo ERROR: HiveMind configure failed. Cleaning...
    if exist "%HiveMind_BUILD%" rmdir /S /Q "%HiveMind_BUILD%"
    pause & exit /b 1
)

"%CM%" --build "%HiveMind_BUILD%" -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 (
    echo ERROR: HiveMind build failed. Cleaning...
    if exist "%HiveMind_BUILD%" rmdir /S /Q "%HiveMind_BUILD%"
    pause & exit /b 1
)

echo.
echo ============================================================
echo  Build complete.
echo  Output: %HiveMind_BUILD%\HiveMind.exe
echo ============================================================
echo.
pause