# make_shortcut.ps1 — a Desktop shortcut to the beta.
#
# Why a shortcut and not a copy of the exe on the Desktop itself: the editor
# loads Shaders/ and Templates/ from paths RELATIVE to its own module, and
# NF_BASIC3D_SHADER_DIR is baked as an absolute path into the build tree. A bare
# exe sitting loose on the Desktop starts, then renders unlit fallbacks and
# refuses to scaffold a project, because its siblings are not there. So the
# staged folder is the unit of distribution and the shortcut is the entry point.
$ErrorActionPreference = 'Stop'

$version = '0.2.0-beta.1'
# Same rule as package_beta.ps1: the real Desktop, not $env:USERPROFILE\Desktop.
# OneDrive redirects the latter on this machine, so a shortcut written there is
# a shortcut the user never sees.
$desktop = [Environment]::GetFolderPath('Desktop')
$stage   = Join-Path $desktop "NOVAForge-$version"
$exe     = Join-Path $stage 'NOVAForgeEditor.exe'
if (-not (Test-Path $exe)) { throw "Stage not found at $stage - run package_beta.ps1 first." }

$lnk = Join-Path $desktop "NOVAForge $version.lnk"

$ws = New-Object -ComObject WScript.Shell
$s  = $ws.CreateShortcut($lnk)
$s.TargetPath       = $exe
$s.WorkingDirectory = $stage
# IconIndex 0 is the icon the exe already carries (the RC resource), so this
# needs no separate .ico path and can never drift out of sync with the binary.
$s.IconLocation     = "$exe,0"
$s.Description      = "NOVAForge Engine $version - editor"
$s.WindowStyle      = 1
$s.Save()

Write-Host "shortcut: $lnk"
Write-Host "target  : $s.TargetPath"
Write-Host "icon    : $s.IconLocation"
