# patch_deps.ps1
#
# Run once after cloning and initialising submodules.
# Patches NuRaft and MSQuic so they build correctly on this toolchain.
#
# Usage:
#   git clone --recurse-submodules <repo>
#   cd MindMesh
#   .\patch_deps.ps1
#   .\build.ps1

$ErrorActionPreference = "Stop"
$ProjectDir   = $PSScriptRoot
$NuRaftCMake  = Join-Path $ProjectDir "NuRaft\CMakeLists.txt"
$MsQuicBuild  = Join-Path $ProjectDir "msquic\scripts\build.ps1"

Write-Host ""
Write-Host "[patch] MindMesh dependency patcher"
Write-Host ""

# ── Verify submodules are present ────────────────────────────
foreach ($file in @($NuRaftCMake, $MsQuicBuild)) {
    if (-not (Test-Path $file)) {
        Write-Error "[ERROR] $file not found.`nRun: git submodule update --init --recursive"
        exit 1
    }
}

# ── Patch 1: NuRaft\CMakeLists.txt ───────────────────────────
# Inject add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
# immediately after cmake_minimum_required(...) so it applies
# globally to all NuRaft targets.
$nuraft = Get-Content $NuRaftCMake -Raw

if ($nuraft -match 'WIN32_LEAN_AND_MEAN') {
    Write-Host "[skip] NuRaft\CMakeLists.txt already patched."
} else {
    $inject = "`nadd_compile_definitions(`n    WIN32_LEAN_AND_MEAN`n    NOMINMAX`n)`n"
    $nuraft = $nuraft -replace '(cmake_minimum_required\s*\([^)]+\))', "`$1$inject"
    Set-Content $NuRaftCMake $nuraft -NoNewline
    Write-Host "[ok] NuRaft\CMakeLists.txt patched."
}

# ── Patch 2: msquic\scripts\build.ps1 ────────────────────────
# Replace "Visual Studio 17 2022" with "Visual Studio 18"
# so MSQuic's cmake invocation targets the correct VS generator.
$msquic = Get-Content $MsQuicBuild -Raw

if ($msquic -match 'Visual Studio 18') {
    Write-Host "[skip] msquic\scripts\build.ps1 already patched."
} else {
    $msquic = $msquic -replace 'Visual Studio 17 2022', 'Visual Studio 18'
    Set-Content $MsQuicBuild $msquic -NoNewline
    Write-Host "[ok] msquic\scripts\build.ps1 patched."
}

Write-Host ""
Write-Host "[done] Dependencies patched."
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Build MSQuic:   cd msquic; .\scripts\build.ps1 -Tls openssl"
Write-Host "  2. Build NuRaft:   cd NuRaft; mkdir build; cd build; cmake ..; cmake --build ."
Write-Host "  3. Build MindMesh: .\build.ps1"
Write-Host ""