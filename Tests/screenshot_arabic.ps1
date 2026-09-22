# Tests/screenshot_arabic.ps1 — visual proof for the Arabic-shell work.
#
# Drives the REAL binaries (no mocks): seeds settings.json, launches
# NOVAForgeEditor (launcher shell), clicks nav/radio rows computed from the
# same layout math as ProjectLauncher.cpp, captures with PrintWindow, then
# relaunches to prove persistence, resizes, and finally captures the ImGui
# editor in Arabic (via --frames automation + --arabic / saved language).
#
# Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File Tests/screenshot_arabic.ps1
#
# Output: <repo>/shots/*.png (also echoed as a file list at the end).

param(
    [string]$OutDir = (Join-Path (Get-Location) 'shots'),
    [string]$Exe = (Join-Path (Get-Location) 'build/DebugNinja/bin/NOVAForgeEditor.exe'),
    [switch]$LauncherOnly,
    [switch]$EditorOnly
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr x);
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr x);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rc);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rc);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT pt);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmd);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int w, int h, bool repaint);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);
    [DllImport("gdi32.dll")] public static extern int GetDeviceCaps(IntPtr hdc, int nIndex);
    public struct RECT { public int left; public int top; public int right; public int bottom; }
    public struct POINT { public int x; public int y; }
}
"@

Add-Type -AssemblyName System.Drawing

# FindWindow by class misses top-level windows when PowerShell marshals $null
# as "" (empty title filter), so enumerate and match the class directly.
function Find-LauncherWindow {
    $found = @()
    $sb = New-Object System.Text.StringBuilder(256)
    $cb = {
        param($h, $x)
        [Win32]::GetClassName($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq 'NOVAForgeProjectLauncher') { $found += $h }
        return $true
    }.GetNewClosure()
    [Win32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($found.Count -gt 0) { return $found[0] }
    return [IntPtr]::Zero
}

$LOGPIXELSX = 88
$WM_CLOSE = 0x10
$MOUSEEVENTF_LEFTDOWN = 0x2
$MOUSEEVENTF_LEFTUP = 0x4
$SW_RESTORE = 9
$SW_MAXIMIZE = 3

function Get-DpiScale {
    $hdc = [Win32]::GetDC([IntPtr]::Zero)
    $dpi = [Win32]::GetDeviceCaps($hdc, $LOGPIXELSX)
    [Win32]::ReleaseDC([IntPtr]::Zero, $hdc) | Out-Null
    if ($dpi -le 0) { $dpi = 96 }
    return $dpi / 96.0
}

function Get-ClientSize($hwnd) {
    $rc = New-Object 'Win32+RECT'
    [Win32]::GetClientRect($hwnd, [ref]$rc) | Out-Null
    # NB: member-access arithmetic must go through locals — `@($rc.right -
    # $rc.left)` mis-binds on this host (op_Subtraction on Object[]).
    $w = $rc.right - $rc.left
    $h = $rc.bottom - $rc.top
    return @($w, $h)
}

function ConvertTo-Screen($hwnd, $cx, $cy) {
    $pt = New-Object 'Win32+POINT'
    $pt.x = [int]$cx
    $pt.y = [int]$cy
    [Win32]::ClientToScreen($hwnd, [ref]$pt) | Out-Null
    $sx = $pt.x
    $sy = $pt.y
    return @($sx, $sy)
}

function Invoke-Click($hwnd, $cx, $cy) {
    $p = ConvertTo-Screen $hwnd $cx $cy
    [Win32]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 150
    [Win32]::SetCursorPos($p[0], $p[1]) | Out-Null
    Start-Sleep -Milliseconds 120
    [Win32]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Win32]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

function Save-Shot($hwnd, $path, [switch]$FromScreen) {
    [Win32]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 400
    $rc = New-Object 'Win32+RECT'
    [Win32]::GetWindowRect($hwnd, [ref]$rc) | Out-Null
    $x0 = $rc.left
    $y0 = $rc.top
    $w = $rc.right - $rc.left
    $h = $rc.bottom - $rc.top
    if ($w -le 0 -or $h -le 0) { throw "zero-size window for $path" }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if ($FromScreen) {
        # Vulkan swapchain content never answers WM_PRINT (blank), so copy
        # the composed screen instead — the window must be foreground.
        $g.CopyFromScreen($x0, $y0, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    } else {
        $hdc = $g.GetHdc()
        [Win32]::PrintWindow($hwnd, $hdc, 0) | Out-Null
        $g.ReleaseHdc($hdc)
    }
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("shot: " + $path)
}

function Wait-ForLauncher($proc, $timeoutSec = 40) {
    # The process's own main-window handle (proven reliable); class search via
    # FindWindow/EnumWindows is flaky from this host.
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $proc.Refresh()
        if ($proc.HasExited) { throw 'launcher process exited early' }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { return $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 500
    }
    throw 'launcher window did not appear'
}

function Wait-ProcessExit($proc, $timeoutSec = 20) {
    if ($proc -ne $null -and -not $proc.HasExited) {
        $proc | Wait-Process -Timeout $timeoutSec -ErrorAction SilentlyContinue
    }
}

function Close-Hwnd($hwnd) {
    [Win32]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}

$VK_CONTROL = 0x11
$VK_ESCAPE = 0x1B
$KEYEVENTF_KEYUP = 0x2

function Press-Key($vk) {
    [Win32]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Win32]::keybd_event([byte]$vk, 0, $KEYEVENTF_KEYUP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
}

function Invoke-Chord($mod, $key) {
    [Win32]::keybd_event([byte]$mod, 0, 0, [UIntPtr]::Zero)
    Press-Key $key
    [Win32]::keybd_event([byte]$mod, 0, $KEYEVENTF_KEYUP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
}

$settingsDir = Join-Path $env:APPDATA 'NOVAForge'
$settingsFile = Join-Path $settingsDir 'settings.json'

function Set-Settings($lang, $maxed) {
    New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null
    $json = ('{"language":"' + $lang + '","maximized":' + $maxed + ',"width":1180,"height":720}')
    Set-Content -LiteralPath $settingsFile -Value $json -Encoding Ascii
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "missing exe: $Exe" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# A stale instance (e.g. the user's live window) would sit on top and swallow
# every synthetic click while captures show the fresh window — clean slate.
Get-Process -Name 'NOVAForgeEditor' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
$s = Get-DpiScale
Write-Host ("dpi scale: " + $s)

if (-not $EditorOnly) {
# --- 1. English baseline -------------------------------------------------------
Set-Settings 'en' 'true'
$proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
try {
    $hwnd = Wait-ForLauncher $proc
    Start-Sleep -Seconds 2
    Save-Shot $hwnd (Join-Path $OutDir '00_projects_en.png')

    # Settings page (nav index 4), still English. Nav clicks go twice:
    # re-showing the current page is a harmless no-op, and it rides over a
    # single flaked synthetic click (foreground race).
    $size = Get-ClientSize $hwnd
    $sw = 200 * $s
    Invoke-Click $hwnd ($sw / 2) ((84 + 4 * 42 + 18) * $s)
    Invoke-Click $hwnd ($sw / 2) ((84 + 4 * 42 + 18) * $s)
    Save-Shot $hwnd (Join-Path $OutDir '01_settings_en.png')

    # --- 2. Live flip to Arabic (the acceptance moment: no restart) ------------
    $size = Get-ClientSize $hwnd
    $W = $size[0]
    $cw = $W - 264 * $s
    $gw = ($cw - 32 * $s) / 3
    $cardL = 232 * $s
    $ryAr = (108 + 86) * $s # settings_geom: gy=108, Arabic row at gy+86
    $rx = $cardL + $gw / 2
    Invoke-Click $hwnd $rx $ryAr
    Invoke-Click $hwnd $rx $ryAr # idempotent: applying Arabic twice is a no-op
    Save-Shot $hwnd (Join-Path $OutDir '02_settings_ar_live.png')

    # --- 3. All six pages in Arabic -------------------------------------------
    # RTL: the sidebar docks RIGHT after the flip, so nav clicks use the
    # mirrored x (this is also the click-matches-paint proof: a wrong x would
    # visibly land on the wrong page).
    $names = @('projects', 'new', 'learn', 'store', 'settings', 'help')
    for ($i = 0; $i -lt 6; $i++) {
        if ($i -ne 4) {
            $sz = Get-ClientSize $hwnd
            Invoke-Click $hwnd ($sz[0] - $sw / 2) ((84 + $i * 42 + 18) * $s)
            # Second tap: navigating to the current page re-shows it harmlessly,
            # so one flaked click cannot desync the whole sequence.
            Invoke-Click $hwnd ($sz[0] - $sw / 2) ((84 + $i * 42 + 18) * $s)
        }
        Save-Shot $hwnd ((Join-Path $OutDir ('03_page_' + $names[$i] + '_ar.png')))
    }

    # --- 3a. Search interaction: back to Projects first (the loop ends on
    # Help), then Ctrl+F focuses, a dead query shows the × and the no-match
    # line, Escape (EDIT subclass) clears it back. US layout assumed for zzz.
    $szS = Get-ClientSize $hwnd
    Invoke-Click $hwnd ($szS[0] - $sw / 2) ((84 + 0 * 42 + 18) * $s)
    Invoke-Click $hwnd ($szS[0] - $sw / 2) ((84 + 0 * 42 + 18) * $s)
    Start-Sleep -Milliseconds 300
    Invoke-Chord $VK_CONTROL 0x46 # Ctrl+F
    foreach ($ch in @([byte][char]'Z', [byte][char]'Z', [byte][char]'Z')) {
        Press-Key $ch # no Shift held: US layout yields lowercase zzz
    }
    Start-Sleep -Milliseconds 500
    Save-Shot $hwnd (Join-Path $OutDir '14_search_nomatch_ar.png')
    Press-Key $VK_ESCAPE
    Start-Sleep -Milliseconds 500
    Save-Shot $hwnd (Join-Path $OutDir '15_search_cleared_ar.png')

    # --- 3b. Forget-entry × (in-memory only: closing without picking never
    # saves the recent file, so the real list is untouched) --------------------
    $szP = Get-ClientSize $hwnd
    $Wp = $szP[0]
    Invoke-Click $hwnd ($Wp - $sw / 2) ((84 + 0 * 42 + 18) * $s) # back to Projects
    $gwp = (($Wp - 264 * $s) - 3 * 16 * $s) / 4
    $xx = $Wp - 214 * $s - $gwp
    Invoke-Click $hwnd $xx (374 * $s) # card × → status + empty CTA
    Save-Shot $hwnd (Join-Path $OutDir '11_removed_ar.png')

    # --- 4. Persistence: close, relaunch, must still be Arabic -----------------
    Close-Hwnd $hwnd
    Wait-ProcessExit $proc
    $saved = Get-Content -LiteralPath $settingsFile -Raw
    Write-Host ("settings after close: " + $saved)
    if ($saved -notmatch '"ar"') { throw 'language did not persist to settings.json' }

    $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
    $hwnd = Wait-ForLauncher $proc
    Start-Sleep -Seconds 2
    Save-Shot $hwnd (Join-Path $OutDir '04_projects_ar_relaunch.png')

    # --- 5. Resize + maximize after the RTL flip -------------------------------
    [Win32]::ShowWindow($hwnd, $SW_RESTORE) | Out-Null
    Start-Sleep -Milliseconds 500
    [Win32]::MoveWindow($hwnd, 100, 100, 1280, 800, $true) | Out-Null
    Start-Sleep -Seconds 1
    Save-Shot $hwnd (Join-Path $OutDir '05_resized_1280x800_ar.png')
    $sz2 = Get-ClientSize $hwnd
    Invoke-Click $hwnd ($sz2[0] - $sw / 2) ((84 + 0 * 42 + 18) * $s) # Projects, still aligned
    Save-Shot $hwnd (Join-Path $OutDir '06_resized_projects_ar.png')

    # --- 5c. Narrow window: responsive columns + "+N more" -------------------------
    # Nine TEMP .nfproj files (the loader prunes missing paths, so fakes must
    # exist) at minimum size: grid drops to 2 columns and the clipped rows
    # announce themselves. Self contained: files + real list restored after.
    $recentFile = Join-Path $env:LOCALAPPDATA 'NOVAForge/recent_projects.txt'
    $fakeBak = $recentFile + '.fakebak'
    $fakeDir = Join-Path $env:TEMP 'nf_fakes'
    $hadFake = Test-Path -LiteralPath $recentFile
    if ($hadFake) {
        Copy-Item -LiteralPath $recentFile -Destination $fakeBak -Force
    }
    New-Item -ItemType Directory -Force -Path $fakeDir | Out-Null
    1..9 | ForEach-Object {
        $fp = Join-Path $fakeDir ("Proj$_.nfproj")
        if (-not (Test-Path -LiteralPath $fp)) {
            New-Item -ItemType File -Path $fp | Out-Null
        }
        $fp
    } | Set-Content -LiteralPath $recentFile -Encoding Ascii
    try {
        # Re-seed Arabic (section-4 close may have persisted anything) and
        # relaunch so the fake list loads fresh.
        Close-Hwnd $hwnd
        Wait-ProcessExit $proc
        Set-Settings 'ar' 'true'
        $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
        $hwnd = Wait-ForLauncher $proc
        Start-Sleep -Seconds 2
        [Win32]::ShowWindow($hwnd, $SW_RESTORE) | Out-Null
        Start-Sleep -Milliseconds 500
        [Win32]::MoveWindow($hwnd, 100, 100, 1100, 720, $true) | Out-Null
        Start-Sleep -Seconds 1
        Save-Shot $hwnd (Join-Path $OutDir '12_narrow_grid_ar.png')
        $sz3 = Get-ClientSize $hwnd
        Invoke-Click $hwnd ($sz3[0] - $sw / 2) ((84 + 2 * 42 + 18) * $s) # Learn, 2-col grid
        Save-Shot $hwnd (Join-Path $OutDir '13_narrow_learn_ar.png')
        Close-Hwnd $hwnd
        Wait-ProcessExit $proc
        $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
        $hwnd = Wait-ForLauncher $proc
        Start-Sleep -Seconds 2
    }
    finally {
        if ($hadFake) {
            Move-Item -LiteralPath $fakeBak -Destination $recentFile -Force
        } elseif (Test-Path -LiteralPath $recentFile) {
            Remove-Item -LiteralPath $recentFile -Force
        }
        Remove-Item -LiteralPath $fakeDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    [Win32]::ShowWindow($hwnd, $SW_MAXIMIZE) | Out-Null
    Start-Sleep -Seconds 1
    Save-Shot $hwnd (Join-Path $OutDir '07_maximized_ar.png')

    Close-Hwnd $hwnd
    Wait-ProcessExit $proc
}
finally {
    if ($proc -ne $null -and -not $proc.HasExited) {
        Stop-Process -InputObject $proc -Force
    }
}

# --- 5b. Empty library CTA (no recents at all) ----------------------------------
# Back up the real recent list, launch with none, prove the empty-state CTA
# (not the no-match line), then restore. try/finally: the backup MUST return.
$recentFile = Join-Path $env:LOCALAPPDATA 'NOVAForge/recent_projects.txt'
$recentBak = $recentFile + '.shotbak'
$hadRecent = Test-Path -LiteralPath $recentFile
if ($hadRecent) {
    Copy-Item -LiteralPath $recentFile -Destination $recentBak -Force
    Remove-Item -LiteralPath $recentFile -Force
}
$procE = $null
try {
    Set-Settings 'ar' 'true'
    $procE = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
    $hwndE = Wait-ForLauncher $procE
    Start-Sleep -Seconds 2
    Save-Shot $hwndE (Join-Path $OutDir '10_empty_ar.png')
    Close-Hwnd $hwndE
    Wait-ProcessExit $procE
}
finally {
    if ($procE -ne $null -and -not $procE.HasExited) {
        Stop-Process -InputObject $procE -Force
    }
    if ($hadRecent) {
        Move-Item -LiteralPath $recentBak -Destination $recentFile -Force
    } elseif (Test-Path -LiteralPath $recentFile) {
        Remove-Item -LiteralPath $recentFile -Force
    }
}
} # end launcher block (skipped with -EditorOnly)

if (-not $LauncherOnly) {
# --- 6. Editor in Arabic --------------------------------------------------------
# --frames runs the automation harness windowed (skips the launcher, uses the
# engine-tree mounts); --arabic localises, and the second run proves the saved
# language applies with no flag at all.
function Wait-MainWindow($proc, $timeoutSec = 60) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { return $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 500
    }
    throw 'editor window did not appear'
}

Set-Settings 'en' 'true'
$ed = Start-Process -FilePath $Exe -ArgumentList @('--frames', '100000', '--arabic') -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
try {
    $ehwnd = Wait-MainWindow $ed
    Start-Sleep -Seconds 45
    $ed.Refresh()
    if ($ed.HasExited) { throw 'editor exited before the capture' }
    Save-Shot $ehwnd (Join-Path $OutDir '08_editor_ar_flag.png') -FromScreen
}
finally {
    if ($ed -ne $null -and -not $ed.HasExited) { Stop-Process -InputObject $ed -Force }
}

Set-Settings 'ar' 'true'
$ed2 = Start-Process -FilePath $Exe -ArgumentList @('--frames', '100000') -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
try {
    $ehwnd2 = Wait-MainWindow $ed2
    Start-Sleep -Seconds 45
    $ed2.Refresh()
    if ($ed2.HasExited) { throw 'editor exited before the capture' }
    Save-Shot $ehwnd2 (Join-Path $OutDir '09_editor_ar_saved.png') -FromScreen
}
finally {
    if ($ed2 -ne $null -and -not $ed2.HasExited) { Stop-Process -InputObject $ed2 -Force }
}

# --- 7. No strays left behind (task requirement) ---------------------------------
Get-Process -Name 'NOVAForgeEditor' -ErrorAction SilentlyContinue | Stop-Process -Force
} # end editor block (skipped with -LauncherOnly)
Write-Host 'done. shots:'
Get-ChildItem -LiteralPath $OutDir -Filter '*.png' | ForEach-Object { Write-Host ('  ' + $_.Name) }
