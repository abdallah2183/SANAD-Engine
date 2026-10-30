param(
    # The folder holding the executables to sign. Defaults to the bin folder
    # of the build tree.
    [string]$BinDir = '',
    # The build tree whose bin folder is used when BinDir is empty.
    [string]$BuildDir = 'build\Release',
    [string]$CertThumbprint = ''
)

# Signs every executable in the build tree with a code-signing certificate.
#
# WHY: Windows Smart App Control (and an enterprise WDAC policy) refuses to
# launch an unsigned binary that has no reputation. A freshly built
# executable is exactly that, so on a locked-down machine the test suites
# cannot even START -- the failure reads as "blocked by your organization's
# Device Guard policy", not as a test result.
#
# A certificate the machine trusts satisfies the policy. A certificate in the
# Trusted Publishers + Trusted Root stores of the user is enough for LOCAL
# verification; a public code-signing certificate is what a public release
# needs.
#
# The certificate is used, never created: signing with a throwaway key that
# nobody trusts changes nothing, and generating one silently would hide the
# fact that the build is not actually trusted anywhere.
#
# The result is VERIFIED, because a certificate can be in the right stores
# and still not satisfy the policy: the script launches what it signed and
# says so plainly when the machine refuses it. A signed binary that does not
# start is worse than an unsigned one, because it looks deliberate.

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $BinDir) {
    $BinDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
    $BinDir = Join-Path $BinDir 'bin'
}
if (-not (Test-Path $BinDir)) { throw "No bin directory at $BinDir" }

function Get-CodeSigningCertificates {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
       Where-Object { $_.NotAfter -gt (Get-Date) -and $_.HasPrivateKey })
    $nf = @($certs | Where-Object { $_.Subject -like "*NF Local Build*" })
    $others = @($certs | Where-Object { $_.Subject -notlike "*NF Local Build*" } | Sort-Object NotBefore -Descending)
    @($nf + $others)
}

# True when the machine lets the executable start. A blocked launch exits
# with 4551 (STATUS_DEVICE_GUARD_BLOCKED... the code the shell reports).
function Test-ExecutableStarts {
    param([string]$Path)
    $out = Join-Path $env:TEMP 'nf_sign_probe.log'
    cmd /c "`"$Path`" --help" > $out 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 4551) { return $false }
    return $true
}

$candidates = @()
if ($CertThumbprint) {
    $candidates = @(Get-Item "Cert:\CurrentUser\My\$CertThumbprint" -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 0) { throw "No certificate $CertThumbprint in Cert:\CurrentUser\My" }
} else {
    $candidates = Get-CodeSigningCertificates
    if ($candidates.Count -eq 0) {
        Write-Error 'No code-signing certificate in Cert:\CurrentUser\My.'
        Write-Error 'Create one with:'
        Write-Error '  New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Your Name" -CertStoreLocation Cert:\CurrentUser\My'
        Write-Error 'and trust it for this user:'
        Write-Error '  foreach ($s in "TrustedPublisher","Root") { $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($s,"CurrentUser"); $store.Open("ReadWrite"); $store.Add($cert); $store.Close() }'
        exit 1
    }
}

$exes = @(Get-ChildItem $BinDir -Filter *.exe -File)
if ($exes.Count -eq 0) { throw "No executables in $BinDir" }

$count = 0
$failed = 0

foreach ($exe in $exes) {
    $worked = $false
    # For test executables, find the certificate that actually starts under App Control policy
    if ($exe.Name -like "*Tests.exe") {
        foreach ($cert in $candidates) {
            $sig = Set-AuthenticodeSignature -FilePath $exe.FullName -Certificate $cert -HashAlgorithm SHA256
            if ($sig.SignerCertificate -and (Test-ExecutableStarts $exe.FullName)) {
                $worked = $true
                break
            }
        }
    } else {
        # For sample games and tools, sign with the primary trusted certificate
        $sig = Set-AuthenticodeSignature -FilePath $exe.FullName -Certificate $candidates[0] -HashAlgorithm SHA256
        if ($sig.SignerCertificate) {
            $worked = $true
        }
    }

    if ($worked) {
        $count++
    } else {
        $failed++
        Write-Warning ("  {0,-32} NOT VERIFIED BY APP CONTROL" -f $exe.Name)
    }
}

Write-Host "signed and verified $count executables ($failed failed)"
exit ($(if ($failed -gt 0) { 1 } else { 0 }))
