param(
    [Parameter(Mandatory = $true)] [string] $Project,
    [Parameter(Mandatory = $true)] [string] $OutputApk,
    [string] $PackageName = '',
    [string] $AppLabel = '',
    [string] $IconPng = '',
    [switch] $Validation
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectPath = (Resolve-Path -LiteralPath $Project).Path
if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf) -or
    [IO.Path]::GetExtension($projectPath) -ine '.nfproj') {
    throw 'Project must point to a .nfproj file.'
}
$destination = [IO.Path]::GetFullPath($OutputApk)
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite $destination" }
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($destination)) -PathType Container)) { throw 'The APK output directory does not exist.' }

# Distinct preview package IDs prevent one project from replacing another on a
# phone. The same project on this computer keeps the ID across preview builds.
if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($projectPath.ToLowerInvariant())
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hashBytes = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    $hash = [BitConverter]::ToString($hashBytes).Replace('-', '').Substring(0, 16).ToLowerInvariant()
    $PackageName = "org.sanadengine.preview.p$hash"
}
if ([string]::IsNullOrWhiteSpace($AppLabel)) {
    $AppLabel = 'SANAD Preview - ' + [IO.Path]::GetFileNameWithoutExtension($projectPath)
}

$nf = Join-Path $repo 'build\Release\bin\nf.exe'
if (-not (Test-Path -LiteralPath $nf -PathType Leaf)) { throw 'Build the SANAD Windows tools first: build_nf.bat Release' }
$stage = Join-Path $repo ('build\AndroidPackaging\project-' + [guid]::NewGuid().ToString('N'))
& $nf build --project $projectPath --out $stage
if ($LASTEXITCODE -ne 0) { throw 'SANAD project build failed.' }

& (Join-Path $PSScriptRoot 'build_preview.ps1') -BuiltProjectDirectory $stage -OutputApk $destination `
    -PackageName $PackageName -AppLabel $AppLabel -IconPng $IconPng -Validation:$Validation
if ($LASTEXITCODE -ne 0) { throw 'Android preview packaging failed.' }
Write-Output "Android package ID: $PackageName"
Write-Output 'This is a debuggable preview, not a store release.'
