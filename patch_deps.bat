@echo off
setlocal EnableDelayedExpansion

:: ============================================================
:: patch_deps.bat
::
:: Run this once after cloning the repository and initialising
:: submodules, before building MSQuic or NuRaft.
::
::   git clone --recurse-submodules <repo>
::   cd MindMesh
::   patch_deps.bat
::   build.bat
:: ============================================================

set "PROJECT=%~dp0"
if "%PROJECT:~-1%"=="\" set "PROJECT=%PROJECT:~0,-1%"

set "NURAFT_CMAKE=%PROJECT%\NuRaft\CMakeLists.txt"
set "MSQUIC_BUILD=%PROJECT%\msquic\scripts\build.ps1"

echo.
echo [patch] MindMesh dependency patcher
echo.

:: ── Verify submodules are present ────────────────────────────
if not exist "%NURAFT_CMAKE%" (
    echo [ERROR] NuRaft\CMakeLists.txt not found.
    echo         Run: git submodule update --init --recursive
    pause
    exit /b 1
)
if not exist "%MSQUIC_BUILD%" (
    echo [ERROR] msquic\scripts\build.ps1 not found.
    echo         Run: git submodule update --init --recursive
    pause
    exit /b 1
)

:: ── Patch 1: NuRaft\CMakeLists.txt ───────────────────────────
:: Inject add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
:: after the cmake_minimum_required line so it applies to all
:: targets NuRaft defines.
::
:: Skip if already patched (idempotent).
findstr /c:"WIN32_LEAN_AND_MEAN" "%NURAFT_CMAKE%" >nul 2>&1
if %errorlevel% equ 0 (
    echo [skip] NuRaft\CMakeLists.txt already patched.
) else (
    powershell -NoProfile -Command ^
        "$f = '%NURAFT_CMAKE%';" ^
        "$content = Get-Content $f -Raw;" ^
        "$insert = \"`nadd_compile_definitions(`n    WIN32_LEAN_AND_MEAN`n    NOMINMAX`n)`n\";" ^
        "$pattern = '(cmake_minimum_required\s*\([^)]+\))';" ^
        "$content = $content -replace $pattern, \"`$1$insert\";" ^
        "Set-Content $f $content -NoNewline;"
    if !errorlevel! neq 0 (
        echo [ERROR] Failed to patch NuRaft\CMakeLists.txt
        pause
        exit /b 1
    )
    echo [ok] NuRaft\CMakeLists.txt patched.
)

:: ── Patch 2: msquic\scripts\build.ps1 ────────────────────────
:: Replace "Visual Studio 17 2022" with "Visual Studio 18 2022"
:: so MSQuic's build script finds the correct VS generator.
::
:: Skip if already patched (idempotent).
findstr /c:"Visual Studio 18" "%MSQUIC_BUILD%" >nul 2>&1
if %errorlevel% equ 0 (
    echo [skip] msquic\scripts\build.ps1 already patched.
) else (
    powershell -NoProfile -Command ^
        "$f = '%MSQUIC_BUILD%';" ^
        "$content = Get-Content $f -Raw;" ^
        "$content = $content -replace 'Visual Studio 17 2022', 'Visual Studio 18 2022';" ^
        "Set-Content $f $content -NoNewline;"
    if !errorlevel! neq 0 (
        echo [ERROR] Failed to patch msquic\scripts\build.ps1
        pause
        exit /b 1
    )
    echo [ok] msquic\scripts\build.ps1 patched.
)

echo.
echo [done] Dependencies patched. You can now build MSQuic and NuRaft,
echo        then run build.bat to build MindMesh.
echo.
echo Next steps:
echo   1. Build MSQuic:   cd msquic ^& powershell .\scripts\build.ps1 -Tls openssl
echo   2. Build NuRaft:   cd NuRaft ^& mkdir build ^& cd build ^& cmake .. ^& cmake --build .
echo   3. Build MindMesh: .\build.bat
echo.
pause