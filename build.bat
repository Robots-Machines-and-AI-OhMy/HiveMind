@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: MindMesh build script
:: Double-click or run from any command prompt.
:: Usage:  build.bat          (Debug)
::         build.bat Release  (Release)
::         build.bat clean    (wipe build folder)
:: ============================================================

set "BUILD_TYPE=Debug"
if /i "%1"=="Release" set "BUILD_TYPE=Release"

set "PROJECT=%~dp0"
if "%PROJECT:~-1%"=="\" set "PROJECT=%PROJECT:~0,-1%"
set "BUILD_DIR=%PROJECT%\build"

:: ── Clean ────────────────────────────────────────────────────
if /i "%1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
    echo Done.
    exit /b 0
)

:: ── Find vcvars64.bat ────────────────────────────────────────
set "VCVARS="
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"      set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"        set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"  set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"  set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo ERROR: Visual Studio Build Tools not found.
    echo Install from: https://visualstudio.microsoft.com/downloads/
    echo Select "Build Tools for Visual Studio" and include the C++ workload.
    pause
    exit /b 1
)

call "%VCVARS%" >nul 2>&1
echo Compiler ready.

:: ── Find cmake ───────────────────────────────────────────────
set "CMAKE_EXE="
if not defined CMAKE_EXE if exist "C:\Program Files\CMake\bin\cmake.exe"       set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files (x86)\CMake\bin\cmake.exe"
if not defined CMAKE_EXE where cmake >nul 2>&1
if not defined CMAKE_EXE set "CMAKE_EXE=cmake"

:: ── Find ninja ───────────────────────────────────────────────
:: Look for VS-bundled ninja first; it is guaranteed x64 and
:: compatible with this MSVC version. Pass it directly to cmake
:: so PATH contents don't matter.
set "NINJA_EXE="
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"    set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"  set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"    set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"    set "NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"   set "NINJA_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined NINJA_EXE if exist "C:\Program Files\CMake\bin\ninja.exe" set "NINJA_EXE=C:\Program Files\CMake\bin\ninja.exe"

if not defined NINJA_EXE (
    echo ERROR: ninja build tool not found.
    echo Install from: https://ninja-build.org/  or run:
    echo   winget install Ninja-build.Ninja
    pause
    exit /b 1
)

:: ── Wipe stale cache ─────────────────────────────────────────
if exist "%BUILD_DIR%\CMakeCache.txt" rmdir /S /Q "%BUILD_DIR%"

echo.
echo Building MindMesh ^(%BUILD_TYPE%^)...
echo.

:: ── Configure ────────────────────────────────────────────────
"%CMAKE_EXE%" -S "%PROJECT%" -B "%BUILD_DIR%" ^
    -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
    -DCMAKE_MT=mt

if %errorlevel% neq 0 (
    echo.
    echo ERROR: Configuration failed. See output above.
    pause
    exit /b 1
)

:: ── Build ────────────────────────────────────────────────────
"%CMAKE_EXE%" --build "%BUILD_DIR%" -j %NUMBER_OF_PROCESSORS%

if %errorlevel% neq 0 (
    echo.
    echo ERROR: Build failed. See output above.
    pause
    exit /b 1
)

echo.
echo Build complete.
echo Output: %BUILD_DIR%\MindMesh.exe
echo.
pause