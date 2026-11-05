<#
.SYNOPSIS
Emit one OSC 7501 Program Status report to the current terminal, for manually
testing axan's sidebar session state.

.DESCRIPTION
Usage: axan-status.ps1 <state> [-Kind permission|question|auth] [-Msg "text"]
                       [-Title "text"] [-Progress N] [-Id a/b] [-App name]
       axan-status.ps1 -Query     send the feature-detection query
       axan-status.ps1 -Demo      walk working -> blocked (question) -> done
state: idle | working | done | blocked | error | clear

GNU-style flags (--kind, --msg, --query, --demo, ...) are accepted too, so the
same command line works for axan-status.sh and axan-status.ps1.
#>
$ErrorActionPreference = 'Stop'

# Normalise GNU-style --flags into PowerShell parameters by hand, so both scripts
# take identical arguments.
$state = $null
$kind = $null; $msg = $null; $title = $null; $progress = $null; $id = $null; $app = $null
$query = $false; $demo = $false
$i = 0
while ($i -lt $args.Count) {
    $a = [string]$args[$i]
    switch -Regex ($a) {
        '^--?(h|help)$' { Get-Help $PSCommandPath; exit 0 }
        '^--?query$' { $query = $true; continue }
        '^--?demo$' { $demo = $true; continue }
        '^--?(kind|msg|title|progress|id|app)$' {
            $name = $a.TrimStart('-').ToLowerInvariant()
            if ($i + 1 -ge $args.Count) { [Console]::Error.WriteLine("axan-status: $a needs a value"); exit 2 }
            $i++
            Set-Variable -Name $name -Value ([string]$args[$i])
            continue
        }
        '^-' { [Console]::Error.WriteLine("axan-status: unknown option $a"); exit 2 }
        default {
            if ($state) { [Console]::Error.WriteLine('axan-status: only one state allowed'); exit 2 }
            $state = $a
        }
    }
    $i++
}

$esc = [char]0x1B
$st = "$esc\"

function ConvertTo-B64([string]$text) {
    [Convert]::ToBase64String([Text.UTF8Encoding]::new($false).GetBytes($text))
}

# Write raw text to stdout as UTF-8 without a BOM or trailing newline, so the bytes
# are exactly the sequence whether the console or a file receives them.
function Write-Raw([string]$text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
    $out = [Console]::OpenStandardOutput()
    $out.Write($bytes, 0, $bytes.Length)
    $out.Flush()
}

function Send-Status([string]$State, $Kind, $Msg, $Title) {
    if ($State -notin 'idle', 'working', 'done', 'blocked', 'error', 'clear') {
        [Console]::Error.WriteLine('axan-status: state must be idle|working|done|blocked|error|clear')
        exit 2
    }
    $pairs = @("state=$State")
    if ($id) { $pairs += "id=$id" }
    if ($app) { $pairs += "app=$app" }
    if ($Kind) { $pairs += "kind=$Kind" }
    if ($progress) { $pairs += "progress=$progress" }
    if ($null -ne $Title) { $pairs += "title=$(ConvertTo-B64 $Title)" }
    if ($null -ne $Msg) { $pairs += "msg=$(ConvertTo-B64 $Msg)" }
    Write-Raw "$esc]7501;$($pairs -join ':')$st"
}

if ($query) {
    Write-Raw "$esc]7501;?$st"
    exit 0
}

if ($demo) {
    [Console]::Error.WriteLine('axan-status demo: working')
    Send-Status 'working' $null 'Refactoring the parser' $title
    Start-Sleep -Seconds 3
    [Console]::Error.WriteLine('axan-status demo: blocked (question)')
    Send-Status 'blocked' 'question' 'Which branch should I target?' $title
    Start-Sleep -Seconds 3
    [Console]::Error.WriteLine('axan-status demo: done')
    Send-Status 'done' $null 'Finished' $title
    [Console]::Error.WriteLine("axan-status demo: end (focus the tab to acknowledge; 'clear' removes it)")
    exit 0
}

if (-not $state) { Get-Help $PSCommandPath; exit 2 }
Send-Status $state $kind $msg $title
