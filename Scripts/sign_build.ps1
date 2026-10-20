param(
    [string]$BuildDir = 'build\Release',
    [string]$CertThumbprint = ''
)

# Signs every executable in the build tree with a code-signing certificate.
#
# WHY: Windows Smart App Control (and an enterprise WDAC policy) refuses to
# launch an unsigned binary that has no reputation. A freshly built
# executable is exactly that, so on a locked-down machine the test suites
# cannot even START -- the failure reads as "blocked by your organization's
# Device Guard policy", not as a test result. Signing the build is the
# standard answer, and it is what a public release needs.
#
# A certificate in the machine's Trusted Publishers + Trusted Root stores
# satisfies the policy. A self-signed one is enough for LOCAL verification;
# a public code-signing certificate is what the release itself needs.
#
# The certificate is used, never created: signing with a throwaway key that
# nobody trusts changes nothing, and generating one silently would hide the
# fact that the build is not actually trusted anywhere.
#
# Set-AuthenticodeSignature embeds the signature and THEN reports whether
# the chain validates. A self-signed development certificate chains to a
# root that only this user trusts, so the chain status is a warning here --
# the signature itself is what the policy looks at.

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$bin = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$bin = Join-Path $bin 'bin'
if (-not (Test-Path $bin)) { throw "No bin directory at $bin" }

if (-not $CertThumbprint) {
    $certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
               Where-Object { $_.NotAfter -gt (Get-Date) })
    if ($certs.Count -eq 0) {
        Write-Error 'No code-signing certificate in Cert:\CurrentUser\My.'
        Write-Error 'Create one with:'
        Write-Error '  New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Your Name" -CertStoreLocation Cert:\CurrentUser\My'
        Write-Error 'and trust it for this user:'
        Write-Error '  foreach ($s in "TrustedPublisher","Root") { $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($s,"CurrentUser"); $store.Open("ReadWrite"); $store.Add($cert); $store.Close() }'
        exit 1
    }
    $CertThumbprint = $certs[0].Thumbprint
}
$cert = Get-Item "Cert:\CurrentUser\My\$CertThumbprint"
Write-Host "signing with: $($cert.Subject) ($($cert.Thumbprint))"

$count = 0
$unsigned = 0
$untrusted = 0
foreach ($exe in Get-ChildItem $bin -Filter *.exe -File) {
    $sig = Set-AuthenticodeSignature -FilePath $exe.FullName -Certificate $cert -HashAlgorithm SHA256
    if (-not $sig.SignerCertificate) {
        Write-Warning ("  {0,-32} NOT SIGNED" -f $exe.Name)
        $unsigned++
        continue
    }
    $count++
    if ($sig.Status -ne 'Valid') {
        # The signature is embedded; the chain does not validate because the
        # root is trusted only for this user. Say so once, not per file.
        $untrusted++
    }
}
Write-Host "signed $count executables ($unsigned failed, $untrusted with an untrusted chain)"
if ($unsigned -gt 0) { exit 1 }
exit 0
