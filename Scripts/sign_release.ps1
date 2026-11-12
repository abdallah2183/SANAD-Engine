param(
    [Parameter(Mandatory)]
    [string[]]$Path,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string]$CertThumbprint,
    [Parameter(Mandatory)]
    [uri]$TimestampUrl,
    [string]$SignTool = ''
)

$ErrorActionPreference = 'Stop'
if (-not $TimestampUrl.IsAbsoluteUri -or $TimestampUrl.Scheme -notin @('http', 'https')) {
    throw 'Supply the RFC 3161 timestamp URL from your signing provider.'
}
$certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertThumbprint" -ErrorAction SilentlyContinue
if (-not $certificate -or -not $certificate.HasPrivateKey -or $certificate.Subject -eq $certificate.Issuer) {
    throw 'Install your publicly trusted code-signing certificate with its key provider in CurrentUser\My.'
}

if (-not $SignTool) {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) {
        $SignTool = $command.Source
    } else {
        $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
        $SignTool = Get-ChildItem -LiteralPath $sdk -Directory -ErrorAction SilentlyContinue |
            Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -First 1
    }
}
if (-not $SignTool -or -not (Test-Path -LiteralPath $SignTool -PathType Leaf)) {
    throw 'Install SignTool from the Windows SDK or supply -SignTool.'
}

$files = @(foreach ($item in $Path) {
    Get-Item -LiteralPath $item
})
foreach ($file in $files) {
    if ($file.PSIsContainer) { throw "Supply an executable file: $($file.FullName)" }
}

foreach ($file in $files) {
    $staged = Join-Path $file.DirectoryName ('.sanad-sign-' + [guid]::NewGuid() + $file.Extension)
    try {
        Copy-Item -LiteralPath $file.FullName -Destination $staged
        & $SignTool sign /sha1 $CertThumbprint /fd SHA256 /tr $TimestampUrl.AbsoluteUri /td SHA256 $staged
        if ($LASTEXITCODE -ne 0) { throw "Signing failed for $($file.FullName)." }
        & $SignTool verify /pa /all /tw $staged
        if ($LASTEXITCODE -ne 0) { throw "Signature or timestamp verification failed for $($file.FullName)." }
        [System.IO.File]::Replace($staged, $file.FullName, [NullString]::Value)
        Write-Host "Signed and verified $($file.FullName)"
    } finally {
        if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged -Force }
    }
}
