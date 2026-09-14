# ---------------------------------------------------------------------------
# NOVAForge Engine — Windows build helper (CMake + Ninja + MSVC)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File Scripts\build.ps1 [-Config Debug]
#
# Why this exists:
#   The usual approach is to call vcvarsall.bat before Ninja, but vcvarsall
#   shells out to reg.exe (blocked in some sandboxed/CI environments) and
#   misparses PATH entries that contain parentheses, e.g.
#   "C:\Program Files (x86)\Nmap". Here the MSVC environment is assembled
#   directly, which is both faster and immune to both problems.
# ---------------------------------------------------------------------------

param(
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

function Write-NF([string]$Message) {
    Write-Output "[NF] $Message"
}

# --- Locate Visual Studio ---------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 or newer."
}

$vsInstall = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $vsInstall) {
    throw "No Visual Studio installation with the C++ toolset was found."
}
$vsInstall = $vsInstall.Trim()

Write-NF "Visual Studio: $vsInstall"

# --- Choose the newest MSVC toolset ----------------------------------------
$msvcParent = Join-Path $vsInstall "VC\Tools\MSVC"
if (-not (Test-Path $msvcParent)) {
    throw "MSVC toolsets not found at $msvcParent"
}

$msvcRoot = Get-ChildItem -Path $msvcParent -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "bin\Hostx64\x64\cl.exe") } |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $msvcRoot) {
    throw "No usable MSVC toolset found under $msvcParent"
}
Write-NF "MSVC toolset:  $($msvcRoot.FullName)"

# --- Choose the newest Windows SDK -----------------------------------------
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10"
if (-not (Test-Path $sdkRoot)) {
    throw "Windows SDK not found at $sdkRoot"
}

$sdkVersion = Get-ChildItem -Path (Join-Path $sdkRoot "Include") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "um\windows.h") } |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $sdkVersion) {
    throw "No usable Windows SDK version found under $sdkRoot\Include"
}
Write-NF "Windows SDK:   $($sdkVersion.Name)"
Write-NF "Build type:    $Config"

# --- CMake / Ninja shipped with Visual Studio ------------------------------
$cmakeDir = Join-Path $vsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaDir = Join-Path $vsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

# --- Assemble the toolchain environment ------------------------------------
$paths = @(
    (Join-Path $msvcRoot.FullName "bin\Hostx64\x64"),
    (Join-Path $sdkRoot "bin\$($sdkVersion.Name)\x64"),
    $cmakeDir,
    $ninjaDir
) + ($env:PATH -split ';')

$env:PATH = ($paths | Where-Object { $_ -and $_.Trim() }) -join ';'

$env:INCLUDE = @(
    (Join-Path $msvcRoot.FullName "include"),
    (Join-Path $sdkRoot "Include\$($sdkVersion.Name)\ucrt"),
    (Join-Path $sdkRoot "Include\$($sdkVersion.Name)\shared"),
    (Join-Path $sdkRoot "Include\$($sdkVersion.Name)\um"),
    (Join-Path $sdkRoot "Include\$($sdkVersion.Name)\winrt"),
    (Join-Path $sdkRoot "Include\$($sdkVersion.Name)\cppwinrt")
) -join ';'

$env:LIB = @(
    (Join-Path $msvcRoot.FullName "lib\x64"),
    (Join-Path $sdkRoot "Lib\$($sdkVersion.Name)\ucrt\x64"),
    (Join-Path $sdkRoot "Lib\$($sdkVersion.Name)\um\x64")
) -join ';'

$env:LIBPATH = @(
    (Join-Path $msvcRoot.FullName "lib\x64"),
    (Join-Path $sdkRoot "UnionMetadata\$($sdkVersion.Name)"),
    (Join-Path $sdkRoot "References\$($sdkVersion.Name)")
) -join ';'

# --- Configure --------------------------------------------------------------
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir    = Join-Path $projectRoot "build\$Config"

Write-NF "Configuring..."
& cmake -S $projectRoot -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed (exit $LASTEXITCODE)."
}

# --- Build ------------------------------------------------------------------
Write-NF "Building..."
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE)."
}

Write-NF "BUILD_SUCCEEDED: $buildDir"
