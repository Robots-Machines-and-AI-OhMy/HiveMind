# build.ps1 — MindMesh build script
#
# Usage:
#   .\build.ps1                  # Debug
#   .\build.ps1 -Config Release  # Release
#   .\build.ps1 -Clean           # Wipe build directory

param(
    [string] $Config = "Debug",
    [switch] $Clean
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$BuildDir   = Join-Path $ProjectDir "build"

if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host "[clean] Removing $BuildDir..."
        Remove-Item -Recurse -Force $BuildDir
    }
    Write-Host "[clean] Done."
    exit 0
}

# ── Locate vcvars64.bat ───────────────────────────────────────
$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\17\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $vcvars) {
    Write-Error "[ERROR] Cannot find vcvars64.bat. Install Visual Studio Build Tools."
    exit 1
}

Write-Host "[build] Activating: $vcvars"

# Import the x64 MSVC environment into the current PowerShell process.
$envLines = cmd /c "`"$vcvars`" && set" 2>&1
foreach ($line in $envLines) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
Write-Host "[build] x64 toolchain activated."

# ── Strip Strawberry Perl from PATH ──────────────────────────
$env:PATH = ($env:PATH -split ';' |
        Where-Object { $_ -notmatch 'Strawberry' } |
        Where-Object { $_ -ne '' }) -join ';'

# ── Locate x64 ninja ─────────────────────────────────────────
$ninjaCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
    "C:\Program Files\CMake\bin"
)
$ninjaDir = $ninjaCandidates |
        Where-Object { Test-Path (Join-Path $_ "ninja.exe") } |
        Select-Object -First 1

if ($ninjaDir) {
    $env:PATH = "$ninjaDir;$env:PATH"
    Write-Host "[build] Ninja: $ninjaDir\ninja.exe"
} else {
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        Write-Error "[ERROR] ninja not found. Run: winget install Ninja-build.Ninja"
        exit 1
    }
    Write-Host "[build] Ninja: found on PATH"
}

# ── Wipe stale cmake cache ────────────────────────────────────
if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) {
    Write-Host "[build] Wiping stale CMake cache..."
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host ""
Write-Host "[build] Config : $Config"
Write-Host "[build] Output : $BuildDir"
Write-Host ""

# ── Configure ─────────────────────────────────────────────────
cmake -S $ProjectDir -B $BuildDir `
      -G Ninja `
      -DCMAKE_BUILD_TYPE=$Config `
      -DCMAKE_C_COMPILER=cl `
      -DCMAKE_CXX_COMPILER=cl `
      -DCMAKE_MT=mt

if ($LASTEXITCODE -ne 0) { Write-Error "[ERROR] Configure failed."; exit 1 }
Write-Host "[ok] Configure done."

# ── Build ─────────────────────────────────────────────────────
Write-Host ""
$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
Write-Host "[build] Building ($cores jobs)..."
cmake --build $BuildDir -j $cores

if ($LASTEXITCODE -ne 0) { Write-Error "[ERROR] Build failed."; exit 1 }

Write-Host ""
Write-Host "[done] $BuildDir\MindMesh.exe"
Write-Host ""