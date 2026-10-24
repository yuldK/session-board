#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$WorkDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot
$WorkDirectory = [IO.Path]::GetFullPath((Join-Path $WorkDirectory ([guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
$data_directory = Join-Path $WorkDirectory '한글 세션'
$checks = 0

function Assert-Condition([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) {
        throw $message
    }
}

function Invoke-Hook([string]$payload, [bool]$strict_mode = $true) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in @('--provider', 'codex', '--data-dir', $data_directory)) {
        $info.ArgumentList.Add($argument)
    }
    if ($strict_mode) {
        $info.ArgumentList.Add('--strict')
    }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $process.StandardInput.Write($payload)
        $process.StandardInput.Close()
        $output = $process.StandardOutput.ReadToEnd()
        $diagnostics = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        Assert-Condition ($process.ExitCode -eq 0) "Hook failed: $diagnostics"
        Assert-Condition ($output.Trim() -eq '{}') 'Hook stdout must contain only an empty JSON object.'
    }
    finally {
        $process.Dispose()
    }
}

$events = @('SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PermissionRequest', 'PostToolUse', 'Stop', 'Interrupt', 'SessionEnd')
foreach ($event_name in $events) {
    $payload = @{ hook_event_name = $event_name; session_id = 'integration-codex'; turn_id = 'turn-a'; cwd = 'E:/개발/테스트'; prompt = '실제 Hook 경로 검증'; tool_name = 'exec_command'; tool_use_id = 'tool-a' }
    Invoke-Hook ($payload | ConvertTo-Json -Compress)
    $snapshot = Get-ChildItem -LiteralPath $data_directory -Filter '*.json' | Get-Content -Raw -Encoding utf8 | ConvertFrom-Json
    $expected = switch ($event_name) {
        'SessionStart' { 'idle' }
        'UserPromptSubmit' { 'running' }
        'PreToolUse' { 'running' }
        'PermissionRequest' { 'waiting_approval' }
        'PostToolUse' { 'running' }
        'Stop' { 'idle' }
        'Interrupt' { 'interrupted' }
        'SessionEnd' { 'ended' }
    }
    Assert-Condition ($snapshot.status -eq $expected) "Wrong state after $event_name"
}
Assert-Condition ($snapshot.title -eq '실제 Hook 경로 검증') 'UTF-8 title was not preserved.'
Invoke-Hook '{ invalid' $false
Assert-Condition (@(Get-ChildItem -LiteralPath $data_directory -Filter '*.json').Count -eq 1) 'Malformed hook changed storage.'

$hooks_file = Join-Path $WorkDirectory 'hooks.json'
$install_directory = Join-Path $WorkDirectory 'installed tools'
$existing = @{
    description = 'preserve this'
    hooks = @{
        SessionStart = @(@{ hooks = @(@{ type = 'command'; command = 'echo existing' }) })
        PostToolUse = @(@{ hooks = @(@{ type = 'mcp_tool'; server_label = 'existing'; tool_name = 'inspect' }) })
    }
    unrelated = @{ preserved = $true }
}
$existing | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $hooks_file -Encoding utf8NoBOM
$installer = Join-Path $project_root 'scripts/install-codex-hooks.ps1'
& $installer -Executable $Executable -HooksFile $hooks_file -InstallDirectory $install_directory | Out-Null
& $installer -Executable $Executable -HooksFile $hooks_file -InstallDirectory $install_directory | Out-Null
$installed = Get-Content -LiteralPath $hooks_file -Raw | ConvertFrom-Json -AsHashtable
Assert-Condition $installed.unrelated.preserved 'Installer removed unrelated config.'
Assert-Condition ($installed.description -eq 'preserve this') 'Installer changed the description.'
Assert-Condition ($installed.hooks.PostToolUse[0].hooks[0].type -eq 'mcp_tool') 'Installer removed an MCP hook.'
foreach ($event_name in $events) {
    $ours = @($installed.hooks[$event_name] | ForEach-Object { $_.hooks } | Where-Object { $_.Contains('command') -and $_.command -match 'session-board-hook' })
    Assert-Condition ($ours.Count -eq 1) "Installer duplicated $event_name"
}
$windows_command = $installed.hooks.SessionStart[-1].hooks[0].commandWindows
$shell_info = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
$shell_info.UseShellExecute = $false
$shell_info.CreateNoWindow = $true
$shell_info.RedirectStandardInput = $true
$shell_info.RedirectStandardOutput = $true
$shell_info.RedirectStandardError = $true
$shell_info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
$shell_info.ArgumentList.Add('-NoProfile')
$shell_info.ArgumentList.Add('-NonInteractive')
$shell_info.ArgumentList.Add('-Command')
$shell_info.ArgumentList.Add($windows_command + " --strict --data-dir '" + $data_directory.Replace("'", "''") + "'")
$shell_process = [Diagnostics.Process]::Start($shell_info)
try {
    $shell_process.StandardInput.Write('{"hook_event_name":"SessionStart","session_id":"integration-shell"}')
    $shell_process.StandardInput.Close()
    $shell_output = $shell_process.StandardOutput.ReadToEnd()
    $shell_error = $shell_process.StandardError.ReadToEnd()
    $shell_process.WaitForExit()
    Assert-Condition ($shell_process.ExitCode -eq 0) "Installed PowerShell hook command failed: $shell_error"
    Assert-Condition ($shell_output.Trim() -eq '{}') 'Installed command returned invalid hook output.'
}
finally {
    $shell_process.Dispose()
}
$before_preview = [IO.File]::ReadAllText($hooks_file)
& $installer -HooksFile $hooks_file -InstallDirectory $install_directory -Preview | Out-Null
Assert-Condition ([IO.File]::ReadAllText($hooks_file) -ceq $before_preview) 'Preview modified the config.'
& $installer -HooksFile $hooks_file -InstallDirectory $install_directory -Remove | Out-Null
$removed = Get-Content -LiteralPath $hooks_file -Raw | ConvertFrom-Json -AsHashtable
Assert-Condition ($removed.hooks.Count -eq 2) 'Remove changed other hook events.'
Assert-Condition ($removed.hooks.SessionStart[0].hooks[0].command -eq 'echo existing') 'Remove deleted an existing command.'
Assert-Condition (@(Get-ChildItem -LiteralPath $WorkDirectory -Filter '*.bak').Count -eq 3) 'Installer did not back up existing config.'
Write-Output "$checks integration checks passed. Fixtures: $WorkDirectory"
