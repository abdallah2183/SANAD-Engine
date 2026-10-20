param(
    # The build tree to verify.
    [string]$BuildDir = 'build\Release',
    # The repository root (derived from this script's location by default).
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSCommandPath)),
    # How long to keep retrying a suite the machine's code-integrity policy
    # refuses to launch. The verdict comes from the cloud and arrives a few
    # minutes after the file is first seen.
    [int]$RetryMinutes = 20,
    [switch]$SkipZip
)

# verify_release.ps1 — the acceptance run for a release.
#
# It answers the four questions a release has to answer, and it answers them
# with the SHIPPED binaries rather than with "the folder looks right":
#
#   1. Do all 26 test suites pass?
#   2. Does TreeWorld play, with a clean Vulkan log?
#   3. Does the portable zip work in a folder that has never seen the engine?
#   4. What is the SHA-256 of what is being published?
#
# A suite the machine refuses to LAUNCH is not a failing suite: Windows Smart
# App Control blocks an unsigned binary with no reputation, and the verdict
# arrives from the cloud a few minutes later. The script retries rather than
# counting that as a failure, and says so at the end.

$ErrorActionPreference = 'Continue'
$bin = Join-Path $RepoRoot "$BuildDir\bin"
if (-not (Test-Path $bin)) { throw "No bin directory at $bin" }

$suites = @(
    'AITests', 'AnimationTests', 'AssetTests', 'AudioTests', 'CoreTests',
    'DestructionTests', 'ECSTests', 'EditorTests', 'FoliageTests', 'GameplayTests',
    'InputTests', 'JobTests', 'NetworkTests', 'PerfTests', 'PhysicsTests',
    'PlatformTests', 'RHITests', 'RuntimeTests', 'SaveTests', 'Scene2DTests',
    'ScriptTests', 'StreamingTests', 'ToolTests', 'UITests', 'VFXTests', 'WaterTests'
)

# --- Sign the build (a signed binary satisfies the code-integrity policy) ---
$signScript = Join-Path $RepoRoot 'Scripts\sign_build.ps1'
if (Test-Path $signScript) {
    Write-Host '=== signing the build ==='
    & powershell -ExecutionPolicy Bypass -File $signScript -BuildDir (Join-Path $RepoRoot $BuildDir) | Select-Object -Last 2
}

# --- Submit the binaries to the reputation service --------------------------
# A freshly built executable is unknown to the machine's cloud reputation
# service, so the code-integrity policy refuses to LAUNCH it. The service
# answers a few minutes after it has seen the file, and a Defender custom
# scan of the build tree is what makes it see them. Without this step the
# suites below stay blocked for the whole run.
Write-Host ''
Write-Host '=== submitting the build tree to the reputation service ==='
$mp = 'C:\ProgramData\Microsoft\Windows Defender\Platform\CurrentVersion\MpCmdRun.exe'
if (Test-Path $mp) {
    & $mp -Scan -ScanType 3 -File $bin -DisableRemediation 2>&1 | Select-Object -Last 2
} else {
    Write-Host '  (Defender platform not found - the suites may stay blocked)'
}

function Invoke-Suite {
    param([string]$Name)
    $out = Join-Path $env:TEMP "nf_verify_$Name.txt"
    Push-Location $bin
    cmd /c "$Name.exe" > $out 2>&1
    $code = $LASTEXITCODE
    Pop-Location
    $summary = (Select-String -Path $out -Pattern 'Passed:' | Select-Object -Last 1).Line
    return [PSCustomObject]@{ Suite = $Name; Exit = $code; Summary = $summary }
}

Write-Host ''
Write-Host '=== the 26 test suites ==='
$results = @{}
$pending = @($suites)
$deadline = (Get-Date).AddMinutes($RetryMinutes)
while ($pending.Count -gt 0) {
    foreach ($s in $pending) {
        $r = Invoke-Suite $s
        if ($r.Exit -eq 0 -or $r.Summary) {
            # A summary means the suite RAN (it may still have failures).
            $results[$s] = $r
            Write-Host ("  {0,-16} {1}" -f $s, $r.Summary)
            $pending = $pending | Where-Object { $_ -ne $s }
        }
    }
    if ($pending.Count -eq 0) { break }
    if ((Get-Date) -ge $deadline) { break }
    $wait = [Math]::Min(60, [int]($deadline - (Get-Date)).TotalSeconds)
    Write-Host ("  ... {0} still blocked by the code-integrity policy, retrying in {1}s" -f ($pending -join ', '), $wait)
    Start-Sleep -Seconds $wait
}

$failedSuites = @($results.Values | Where-Object { $_.Exit -ne 0 })
$blockedSuites = @($pending)

# --- TreeWorld, played by the shipped player ---------------------------------
Write-Host ''
Write-Host '=== TreeWorld (the shipped player) ==='
$treeworld = Join-Path $RepoRoot 'Games\TreeWorld'
$nf = Join-Path $bin 'nf.exe'
if (Test-Path $nf) {
    & $nf build --project (Join-Path $treeworld 'TreeWorld.nfproj') | Select-Object -Last 3
}
$player = Join-Path $treeworld 'dist\NFPlayer.exe'
if (Test-Path $player) {
    Push-Location (Join-Path $treeworld 'dist')
    cmd /c 'NFPlayer.exe --project TreeWorld.nfproj --frames 120 --headless --validation' > treeworld.log 2>&1
    $treeCode = $LASTEXITCODE
    Pop-Location
    $treeLog = Join-Path $treeworld 'dist\treeworld.log'
    Get-Content $treeLog -Tail 8
    $treeOk = ($treeCode -eq 0) -and
              (Select-String -Path $treeLog -Pattern 'Validation errors: 0' -Quiet) -and
              (Select-String -Path $treeLog -Pattern 'Alive RHI objects before shutdown: 0' -Quiet)
} else {
    $treeOk = $false
    Write-Host '  TreeWorld was not packaged.'
}

# --- The portable zip, in a folder that has never seen the engine -------------
$zipOk = $false
if (-not $SkipZip) {
    Write-Host ''
    Write-Host '=== the portable zip, extracted into a clean folder ==='
    $zip = Get-ChildItem (Join-Path $RepoRoot 'dist') -Filter 'SANAD-*-Windows-x64-Portable.zip' -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($zip) {
        $clean = Join-Path $env:TEMP 'nf_portable_verify'
        Remove-Item $clean -Recurse -Force -ErrorAction SilentlyContinue
        Expand-Archive -Path $zip.FullName -DestinationPath $clean -Force
        # The extracted binaries are new files to the machine, so they have no
        # reputation yet. Submit them and give the service a few minutes.
        if (Test-Path $mp) {
            & $mp -Scan -ScanType 3 -File $clean -DisableRemediation 2>&1 | Select-Object -Last 1
        }
        $zipNf = Get-ChildItem $clean -Recurse -Filter 'nf.exe' | Select-Object -First 1
        if ($zipNf) {
            # Wait for the code-integrity policy to allow the extracted CLI.
            $zipDeadline = (Get-Date).AddMinutes(10)
            while ((Get-Date) -lt $zipDeadline) {
                $p = Start-Process -FilePath $zipNf.FullName -ArgumentList '--help' -NoNewWindow -Wait -PassThru -ErrorAction SilentlyContinue
                if ($p -and $p.ExitCode -ne 4551) { break }
                Write-Host '  the extracted CLI is blocked by the code-integrity policy, retrying...'
                if (Test-Path $mp) {
                    & $mp -Scan -ScanType 3 -File $zipNf.FullName -DisableRemediation 2>&1 | Out-Null
                }
                Start-Sleep -Seconds 40
            }
            $game = Join-Path $clean 'VerifyGame'
            & $zipNf.FullName new $game --name VerifyGame --template ThirdPerson | Select-Object -Last 2
            & $zipNf.FullName build --project (Join-Path $game 'VerifyGame.nfproj') | Select-Object -Last 3
            $zipPlayer = Get-ChildItem (Join-Path $game 'dist') -Filter 'NFPlayer.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($zipPlayer) {
                # Same for the packaged player.
                $playerDeadline = (Get-Date).AddMinutes(10)
                while ((Get-Date) -lt $playerDeadline) {
                    if (Test-Path $mp) {
                        & $mp -Scan -ScanType 3 -File $zipPlayer.FullName -DisableRemediation 2>&1 | Out-Null
                    }
                    Push-Location (Join-Path $game 'dist')
                    cmd /c 'NFPlayer.exe --frames 60 --headless --validation' > zip_player.log 2>&1
                    $zipCode = $LASTEXITCODE
                    Pop-Location
                    if ($zipCode -ne 4551) { break }
                    Write-Host '  the packaged player is blocked by the code-integrity policy, retrying...'
                    Start-Sleep -Seconds 40
                }
                $zipLog = Join-Path $game 'dist\zip_player.log'
                Get-Content $zipLog -Tail 6
                $zipOk = ($zipCode -eq 0) -and
                         (Select-String -Path $zipLog -Pattern 'Validation errors: 0' -Quiet) -and
                         (Select-String -Path $zipLog -Pattern 'Alive RHI objects before shutdown: 0' -Quiet)
            }
        }
    } else {
        Write-Host '  No portable zip in dist/.'
    }
}

# --- The checksums -----------------------------------------------------------
Write-Host ''
Write-Host '=== SHA-256 ==='
$sums = @()
foreach ($f in Get-ChildItem (Join-Path $RepoRoot 'dist') -Filter 'SANAD-*.zip' -ErrorAction SilentlyContinue |
         Sort-Object Name) {
    $hash = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    $sums += ("{0}  {1}" -f $hash, $f.Name)
    Write-Host ("  {0}  {1}" -f $hash, $f.Name)
}
if ($sums.Count -gt 0) {
    $sums | Set-Content -Path (Join-Path $RepoRoot 'dist\SHA256SUMS.txt') -Encoding ascii
}

# --- The verdict -------------------------------------------------------------
Write-Host ''
Write-Host '==================== SUMMARY ===================='
Write-Host ("suites run     : {0} of 26" -f $results.Count)
$passed = 0; $failed = 0; $skipped = 0
foreach ($r in $results.Values) {
    if ($r.Summary -match 'Passed:\s*(\d+)\s*\|\s*Failed:\s*(\d+)\s*\|\s*Skipped:\s*(\d+)') {
        $passed += [int]$Matches[1]; $failed += [int]$Matches[2]; $skipped += [int]$Matches[3]
    }
}
Write-Host ("tests          : {0} passed / {1} failed / {2} skipped" -f $passed, $failed, $skipped)
Write-Host ("suites failing : {0}" -f $(if ($failedSuites.Count) { ($failedSuites | ForEach-Object { $_.Suite }) -join ', ' } else { 'none' }))
Write-Host ("suites blocked : {0}" -f $(if ($blockedSuites.Count) { $blockedSuites -join ', ' } else { 'none' }))
Write-Host ("TreeWorld      : {0}" -f $(if ($treeOk) { 'OK (120 frames, validation clean, no leaks)' } else { 'FAILED' }))
Write-Host ("portable zip   : {0}" -f $(if ($zipOk) { 'OK (extracted, built and played a game)' } else { 'FAILED' }))

$ok = ($failed -eq 0) -and $failedSuites.Count -eq 0 -and $blockedSuites.Count -eq 0 -and $treeOk -and $zipOk
Write-Host ("RESULT         : {0}" -f $(if ($ok) { 'GREEN' } else { 'NOT GREEN' }))
exit ($(if ($ok) { 0 } else { 1 }))
