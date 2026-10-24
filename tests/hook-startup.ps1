#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [string]$UiaScript = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. $UiaScript
$project_root = Split-Path -Parent $PSScriptRoot
$test_root = Join-Path $project_root ('build/hook-startup-' + [guid]::NewGuid().ToString('N'))
$codex_home = Join-Path $test_root 'codex'
$claude_home = Join-Path $test_root 'claude'
$codex_settings = Join-Path $codex_home 'hooks.json'
$claude_settings = Join-Path $claude_home 'settings.json'
New-Item -ItemType Directory -Path $codex_home,$claude_home -Force | Out-Null
$original_claude = @{permissions=@{deny=@('Bash(git push)')};extra=@{preserve=$true};hooks=@{PreToolUse=@(@{matcher='Bash';hooks=@(@{type='command';command='echo existing'})})}}
$original_claude | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $claude_settings -Encoding utf8NoBOM
$old_codex = $env:CODEX_HOME
$old_claude = $env:CLAUDE_CONFIG_DIR
$records = [Collections.Generic.List[object]]::new()
try {
    $env:CODEX_HOME = $codex_home
    $env:CLAUDE_CONFIG_DIR = $claude_home
    $first_codex = ''
    $first_claude = ''
    for ($run = 1; $run -le 2; $run++) {
        try {
            $null = Connect-LuilApp -isolated -process_name 'session-board' -path (Resolve-Path -LiteralPath $Executable).Path
            $signal = Invoke-LuilStep { (Wait-LuilElement -id 'widget-signal').Current.Name }
            if (-not (Test-Path -LiteralPath $codex_settings)) { throw 'Starting the app did not create Codex hook configuration.' }
            $codex_text = Get-Content -LiteralPath $codex_settings -Raw
            $claude_text = Get-Content -LiteralPath $claude_settings -Raw
            $codex = $codex_text | ConvertFrom-Json -AsHashtable
            $claude = $claude_text | ConvertFrom-Json -AsHashtable
            $codex_count = @($codex.hooks.Values | ForEach-Object { $_ } | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'session-board-hook.+--provider codex$' }).Count
            $claude_count = @($claude.hooks.Values | ForEach-Object { $_ } | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'session-board-hook.+--provider claude$' }).Count
            if ($codex_count -ne 8 -or $claude_count -ne 10) { throw "Incorrect automatic registration: Codex $codex_count, Claude $claude_count" }
            if (-not $claude.extra.preserve -or $claude.permissions.deny[0] -ne 'Bash(git push)' -or $claude.hooks.PreToolUse[0].hooks[0].command -ne 'echo existing') {
                throw 'Automatic registration changed unrelated Claude settings.'
            }
            if ($run -eq 1) {
                if ($signal -notmatch '/hooks 신뢰 필요') { throw 'First launch did not explain the required Codex trust review.' }
                $first_codex = $codex_text
                $first_claude = $claude_text
            }
            elseif ($codex_text -cne $first_codex -or $claude_text -cne $first_claude) {
                throw 'Repeated application launch rewrote hook definitions.'
            }
            $records.Add(@{run=$run;codex_hooks=$codex_count;claude_hooks=$claude_count;signal=$signal;other_settings_preserved=$true})
        }
        finally { Disconnect-LuilApp }
    }
    $records.ToArray() | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $test_root 'verification.json') -Encoding utf8NoBOM
    Write-Output 'App startup passed: fresh Codex registration, Claude merge, trust notice, repeated-launch idempotence.'
    Write-Output (Join-Path $test_root 'verification.json')
}
finally {
    $env:CODEX_HOME = $old_codex
    $env:CLAUDE_CONFIG_DIR = $old_claude
}
