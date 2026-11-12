param(
    [Parameter(Mandatory = $true)] [string] $BuiltProjectDirectory,
    [Parameter(Mandatory = $true)] [string] $OutputApk,
    [string] $PackageName = 'org.sanadengine.preview',
    [string] $AppLabel = 'SANAD Preview',
    [string] $IconPng = '',
    [switch] $Validation
)
$ErrorActionPreference = 'Stop'
$PackageName = $PackageName.ToLowerInvariant()
if ($PackageName -cnotmatch '^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$') { throw 'Invalid Android package name.' }
if ([string]::IsNullOrWhiteSpace($AppLabel) -or $AppLabel.Length -gt 100) { throw 'The Android app label must contain 1 to 100 characters.' }
$destination = [IO.Path]::GetFullPath($OutputApk)
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite $destination" }
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($destination)) -PathType Container)) { throw 'The APK output directory does not exist.' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($IconPng)) { $IconPng = Join-Path $repo 'Editor\resources\SANAD.png' }
$IconPng = (Resolve-Path -LiteralPath $IconPng).Path
if (-not (Test-Path -LiteralPath $IconPng -PathType Leaf) -or [IO.Path]::GetExtension($IconPng) -ine '.png') { throw 'The Android icon must be a PNG file.' }
$sdk = Join-Path $repo 'build\android-sdk'
$buildTools = Join-Path $sdk 'build-tools\36.0.0'
$ndk = Join-Path $sdk 'ndk\29.0.14206865'
$projectDir = (Resolve-Path -LiteralPath $BuiltProjectDirectory).Path
$projects = @(Get-ChildItem -LiteralPath $projectDir -File -Filter '*.nfproj')
if ($projects.Count -ne 1) { throw 'The built project folder must contain exactly one .nfproj file.' }
$libs = @{
    'arm64-v8a' = Join-Path $repo 'build\Android-arm64\lib\libsanad_android.so'
    'armeabi-v7a' = Join-Path $repo 'build\Android-armv7\lib\libsanad_android.so'
}
foreach ($abi in $libs.Keys) {
    if (-not (Test-Path -LiteralPath $libs[$abi] -PathType Leaf)) { throw "Build the $abi Android library before packaging." }
}
$stage = Join-Path $repo ('build\AndroidPackaging\preview-' + [guid]::NewGuid().ToString('N'))
$game = Join-Path $stage 'assets\game'
New-Item -ItemType Directory -Path $game -Force | Out-Null
function Get-GameAssetPath([string] $FullName) {
    $prefix = $game.TrimEnd([char[]] @('\', '/')) + [IO.Path]::DirectorySeparatorChar
    if (-not $FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Asset is outside the staging folder.' }
    return $FullName.Substring($prefix.Length).Replace('\', '/')
}
foreach ($dir in @('Content', 'Cache', 'Shaders')) {
    $source = Join-Path $projectDir $dir
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Missing built game folder: $source" }
    Copy-Item -LiteralPath $source -Destination $game -Recurse
}
Copy-Item -LiteralPath $projects[0].FullName -Destination (Join-Path $game 'project.nfproj')
if ($Validation) { [IO.File]::WriteAllText((Join-Path $game 'validation.enable'), '1') }
$paths = @(Get-ChildItem -LiteralPath $game -File -Recurse | ForEach-Object {
    Get-GameAssetPath $_.FullName
} | Sort-Object)
if ($paths | Where-Object { $_ -match '[\r\n]' }) { throw 'Asset filenames containing line breaks are not supported.' }
[IO.File]::WriteAllLines((Join-Path $game 'manifest.txt'), $paths, [Text.UTF8Encoding]::new($false))
$unsigned = Join-Path $stage 'unsigned.apk'
$aligned = Join-Path $stage 'aligned.apk'
$resRoot = Join-Path $stage 'res'
$drawable = Join-Path $resRoot 'drawable-nodpi'
New-Item -ItemType Directory -Path $drawable -Force | Out-Null
Copy-Item -LiteralPath $IconPng -Destination (Join-Path $drawable 'sanad_icon.png')
$compiledResources = Join-Path $stage 'compiled-resources.zip'
& (Join-Path $buildTools 'aapt2.exe') compile --dir $resRoot -o $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'Android icon compilation failed.' }
$manifestPath = Join-Path $stage 'AndroidManifest.xml'
[xml] $androidManifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'AndroidManifest.xml'))
$androidNamespace = 'http://schemas.android.com/apk/res/android'
$androidManifest.manifest.SetAttribute('package', $PackageName)
$androidManifest.manifest.application.SetAttribute('label', $androidNamespace, $AppLabel) | Out-Null
$androidManifest.manifest.application.SetAttribute('icon', $androidNamespace, '@drawable/sanad_icon') | Out-Null
$androidManifest.manifest.application.activity.SetAttribute('label', $androidNamespace, $AppLabel) | Out-Null
$androidManifest.Save($manifestPath)
& (Join-Path $buildTools 'aapt2.exe') link -I (Join-Path $sdk 'platforms\android-36\android.jar') --manifest $manifestPath -o $unsigned $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'Android resource packaging failed.' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($unsigned, [IO.Compression.ZipArchiveMode]::Update)
try {
    # aapt2 on Windows may preserve backslashes for -A assets; Android asset
    # names require forward slashes, so add them with explicit ZIP paths.
    foreach ($file in Get-ChildItem -LiteralPath $game -File -Recurse) {
        $relative = Get-GameAssetPath $file.FullName
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "assets/game/$relative", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    foreach ($abi in $libs.Keys) {
        if (-not (Test-Path -LiteralPath $libs[$abi])) { continue }
        $stripped = Join-Path $stage ("$abi.so")
        Copy-Item -LiteralPath $libs[$abi] -Destination $stripped
        & (Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe') --strip-unneeded $stripped
        if ($LASTEXITCODE -ne 0) { throw "Failed to strip $abi library." }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $stripped, "lib/$abi/libsanad_android.so", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        if ($Validation) {
            $layer = Join-Path $sdk "vulkan-validation\android-binaries-1.4.363.0\$abi\libVkLayer_khronos_validation.so"
            if (-not (Test-Path -LiteralPath $layer)) { throw "Missing Vulkan validation layer for $abi." }
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $layer, "lib/$abi/libVkLayer_khronos_validation.so", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
} finally { $archive.Dispose() }
& (Join-Path $buildTools 'zipalign.exe') -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'APK alignment failed.' }
$key = Join-Path $sdk 'sanad-preview-debug.keystore'
if (-not (Test-Path -LiteralPath $key)) {
    & (Join-Path $env:JAVA_HOME 'bin\keytool.exe') -genkeypair -keystore $key -storepass android -keypass android -alias androiddebugkey -dname 'CN=SANAD Debug,O=SANAD,C=US' -keyalg RSA -validity 10000
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the local debug key.' }
}
& (Join-Path $buildTools 'apksigner.bat') sign --ks $key --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out $destination $aligned
if ($LASTEXITCODE -ne 0) { throw 'APK signing failed.' }
& (Join-Path $buildTools 'apksigner.bat') verify $destination
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed.' }
Write-Output "Debug preview APK: $destination"
