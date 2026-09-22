# Tests/check_ui_literals.ps1 — static UI-literal gate (Arabic-shell work).
#
# Fails (exit 1) when an ImGui widget call in the editor UI carries a raw Latin
# literal that should have been a localization key. Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File Tests/check_ui_literals.ps1
#
# Scope (documented, not accidental):
# - Only Editor/src/ui/*.cpp ImGui:: widget calls. The GDI launcher shell is
#   covered by unit tests (localization_launcher_keys_resolve) + screenshots;
#   its remaining L"" literals are brand/symbol escapes, not UI wording.
# - Line-based: the first literal of a widget call sits on the ImGui:: line in
#   this codebase; a literal hiding on a continuation line would be missed.
# - printf verbs (%s, %zu, %.2f, ...) are stripped before the Latin check, as
#   are ImGui ###/## id suffixes ("###NFSettings" is an id, not wording).
# - SI unit symbols (ms/us) are language-neutral and stripped as well.
#
# Allowlist (each entry is a deliberate decision, see the test files):
# - NOVAForge ............ product brand (never translated)
# - content:// ........... technical URI prefix (paths, not wording)
# - Ctrl+/Del ............ physical keyboard shortcuts (printed on the keys)
# - viewport lit=, Frame %, GPU %, Render CPU, Memory:, session events,
#   [import # ............ profiler/debug diagnostics (technical counters)
# - no widget for this type, unregistered enum ... reflected-inspector
#   diagnostics for unsupported metadata (developer-facing, like log lines)

param(
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
)

$ErrorActionPreference = 'Stop'

$files = @(
    (Join-Path $RepoRoot 'Editor/src/ui/Toolbar.cpp'),
    (Join-Path $RepoRoot 'Editor/src/ui/Panels.cpp')
)

$widgetCall = 'ImGui::(Text|TextDisabled|TextWrapped|TextUnformatted|TextColored|Button|SmallButton|MenuItem|Checkbox|RadioButton|Combo|Begin|BeginMenu|BeginChild|InputText|InputTextWithHint|DragFloat|DragFloat3|SliderFloat|SliderInt|ColorEdit3|Selectable|SeparatorText|SetTooltip|BulletText)\s*\('
$literal = '"((?:[^"\\]|\\.)*)"'
$printfVerb = '%[-+ #0]*(\d+)?(\.\d+|\.\*)?(hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%]'
$imguiId = '##[^\s"]*'
$unitWord = '\b(ms|us)\b'

$allowLine = @(
    'NOVAForge',
    'content://',
    'Ctrl\+',
    '"Del"',
    'viewport lit=',
    'Frame %',
    'GPU %',
    'Render CPU',
    'Memory:',
    'session events',
    '\[import #',
    'no widget for this type',
    'unregistered enum'
)

# A literal that is an argument of AV()/AVF()/tr()/ui::tr() is a localization
# KEY reference (the correct pattern), not a raw display string.
$keyCall = '(AV|AVF|tr|ui::tr|ui::shape_arabic)\(\s*$'

$failures = @()

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Error " Missing file: $file"
    }
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $file)) {
        $lineNo++
        if ($line -notmatch $widgetCall) {
            continue
        }
        foreach ($m in ([regex]::Matches($line, $literal))) {
            $prefix = $line.Substring(0, $m.Index)
            if ($prefix -match $keyCall) {
                continue # AV("key") — a key reference, exactly the pattern we want
            }
            $text = $m.Groups[1].Value
            $text = [regex]::Replace($text, $imguiId, '')
            $text = [regex]::Replace($text, $printfVerb, '')
            $text = [regex]::Replace($text, $unitWord, '')
            if ($text -match '[A-Za-z]') {
                $allowed = $false
                foreach ($a in $allowLine) {
                    if ($line -match $a) {
                        $allowed = $true
                        break
                    }
                }
                if (-not $allowed) {
                    $failures += ("{0}:{1}: raw Latin literal in widget call: {2}" -f $file, $lineNo, $m.Value)
                }
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'UI literal check FAILED:'
    foreach ($f in $failures) {
        Write-Host ("  " + $f)
    }
    exit 1
}

# Second pass: a Latin-led label before ### is a widget label with an inline
# id ("Translate###gizmo_move") — raw by construction, whatever the call looks
# like (this caught the viewport gizmo buttons, whose literal sits in a helper
# argument instead of on an ImGui:: line).
$labelId = '"[A-Za-z][^"]*###'
foreach ($file in $files) {
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $file)) {
        $lineNo++
        foreach ($m in ([regex]::Matches($line, $labelId))) {
            # Quoted text in a // comment (e.g. "label###id" documenting the
            # pattern) is not code: ignore matches after a comment start.
            $prefix = $line.Substring(0, $m.Index).Replace('://', '  ')
            if ($prefix -match '//') { continue }
            $failures += ("{0}:{1}: raw Latin widget label with inline id: {2}" -f $file, $lineNo, $m.Value)
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'UI literal check FAILED:'
    foreach ($f in $failures) {
        Write-Host ("  " + $f)
    }
    exit 1
}

Write-Host 'UI literal check passed.'
exit 0
