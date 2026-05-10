# detect_toolchain.ps1
# Locates the x64 MSVC toolchain, Windows SDK, ninja, and cmake.
# Writes KEY=VALUE pairs to $env:TEMP\hm_detect.txt for build.bat to consume.

$out = [System.Collections.Generic.List[string]]::new()

# ── MSVC ─────────────────────────────────────────────────────
$msvcBases = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC'
)

$msvcVer = $null
foreach ($base in $msvcBases) {
    if (Test-Path $base) {
        $msvcVer = Get-ChildItem $base -Directory |
                Sort-Object Name -Descending |
                Select-Object -First 1 -ExpandProperty FullName
        if ($msvcVer) { break }
    }
}

if (-not $msvcVer) {
    Write-Host "ERROR: MSVC not found. Install Visual Studio Build Tools."
    exit 1
}

$out.Add("CL_EXE=$msvcVer\bin\Hostx64\x64\cl.exe")
$out.Add("LINK_EXE=$msvcVer\bin\Hostx64\x64\link.exe")
$out.Add("MSVC_BIN=$msvcVer\bin\Hostx64\x64")
$out.Add("MSVC_INC=$msvcVer\include")
$out.Add("MSVC_LIB=$msvcVer\lib\x64")

# ── Windows SDK ───────────────────────────────────────────────
$sdkBases = @(
    'C:\Program Files (x86)\Windows Kits\10\bin',
    'C:\Program Files\Windows Kits\10\bin'
)

$sdkVer = $null
foreach ($b in $sdkBases) {
    if (Test-Path $b) {
        $sdkVer = Get-ChildItem $b -Directory |
                Where-Object { $_.Name -match '^\d' } |
                Sort-Object Name -Descending |
                Select-Object -First 1
        if ($sdkVer) { break }
    }
}

if ($sdkVer) {
    $out.Add("MT_EXE=$($sdkVer.FullName)\x64\mt.exe")
    $out.Add("RC_EXE=$($sdkVer.FullName)\x64\rc.exe")
    $sdkRoot = $sdkVer.FullName | Split-Path | Split-Path
    $out.Add("WINSDK_INC=$sdkRoot\Include\$($sdkVer.Name)")
    $out.Add("WINSDK_LIB=$sdkRoot\Lib\$($sdkVer.Name)")
} else {
    Write-Host "WARNING: Windows SDK not found. Build may fail."
}

# ── Ninja ─────────────────────────────────────────────────────
$ninjaDirs = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja',
    'C:\Program Files\CMake\bin'
)

$ninjaDir = $ninjaDirs | Where-Object { Test-Path "$_\ninja.exe" } | Select-Object -First 1
if (-not $ninjaDir) {
    Write-Host "ERROR: ninja not found. Run: winget install Ninja-build.Ninja"
    exit 1
}
$out.Add("NINJA_DIR=$ninjaDir")

# ── CMake ─────────────────────────────────────────────────────
$cmakePaths = @(
    'C:\Program Files\CMake\bin\cmake.exe',
    'C:\Program Files (x86)\CMake\bin\cmake.exe'
)
$cmakeExe = $cmakePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cmakeExe) { $cmakeExe = 'cmake' }
$out.Add("CM=$cmakeExe")

# ── Write results ─────────────────────────────────────────────
$out | Set-Content "$env:TEMP\hm_detect.txt"
Write-Host "Toolchain detection complete."