#requires -Version 7.3
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'SessionBoard/bin'),
    [string]$SettingsFile,
    [int]$Timeout = 5,
    [switch]$Preview,
    [switch]$Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $SettingsFile) {
    $claude_directory = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
    $SettingsFile = Join-Path $claude_directory 'settings.json'
}
$SettingsFile = [IO.Path]::GetFullPath($SettingsFile)
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$installed_executable = Join-Path $InstallDirectory 'session-board.exe'
# 앱(src/core/claude_hooks.cpp)도 시작할 때 같은 항목을 자동 등록한다. 이 스크립트는 Preview·수동 설치·제거용이다.
# PowerShell 7.3 이상은 순서를 보존하므로 기존 설정 키의 순서와 알 수 없는 키를 그대로 둔다.
$document = if (Test-Path -LiteralPath $SettingsFile) { Get-Content -LiteralPath $SettingsFile -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable } else { [ordered]@{} }
if ($null -eq $document) {
    $document = [ordered]@{}
}
if (-not $document.Contains('hooks')) {
    $document.hooks = [ordered]@{}
}

# 공백 없는 경로는 따옴표 없이 써서 Git Bash와 PowerShell 어느 Hook 셸에서도 그대로 실행되게 한다.
$executable_text = $installed_executable.Replace('\', '/')
$command = if ($executable_text.Contains(' ')) { '"' + $executable_text + '" --hook --provider claude' } else { $executable_text + ' --hook --provider claude' }
$events = [ordered]@{
    SessionStart       = $null
    UserPromptSubmit   = $null
    PreToolUse         = '*'
    PostToolUse        = '*'
    PostToolUseFailure = '*'
    PermissionRequest  = '*'
    Notification       = 'elicitation_dialog|elicitation_url_dialog'
    Stop               = $null
    StopFailure        = $null
    SessionEnd         = $null
}
foreach ($event_name in $events.Keys) {
    $groups = [Collections.Generic.List[object]]::new()
    if ($document.hooks.Contains($event_name)) {
        foreach ($group in @($document.hooks[$event_name])) {
            $handlers = @($group.hooks | Where-Object { -not ($_.Contains('command') -and $_.command -match 'session-board(-hook)?\.exe"?\s+(--hook\s+)?--provider claude$') })
            if ($handlers.Count -gt 0) {
                $group.hooks = $handlers
                $groups.Add($group)
            }
        }
    }
    if (-not $Remove) {
        $group = [ordered]@{}
        if ($null -ne $events[$event_name]) {
            $group.matcher = $events[$event_name]
        }
        $group.hooks = @([ordered]@{ type = 'command'; command = $command; timeout = $Timeout })
        $groups.Add($group)
    }
    if ($groups.Count -gt 0) {
        $document.hooks[$event_name] = @($groups.ToArray())
    }
    else {
        $document.hooks.Remove($event_name)
    }
}
if ($document.hooks.Count -eq 0) {
    $document.Remove('hooks')
}
$json_text = ($document | ConvertTo-Json -Depth 50).Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n"
if ($Preview) {
    Write-Output "Settings: $SettingsFile"
    Write-Output "Command: $command"
    Write-Output $json_text
    return
}
if (-not $Remove) {
    $source_executable = (Resolve-Path -LiteralPath $Executable).Path
    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    $same = (Test-Path -LiteralPath $installed_executable) -and (Get-FileHash -LiteralPath $source_executable).Hash -eq (Get-FileHash -LiteralPath $installed_executable).Hash
    if (-not $same -and $source_executable -ne $installed_executable) {
        # 공유 helper는 Codex Hook도 쓰므로 짧게 실행 중일 수 있다. 잠시 기다렸다 다시 복사한다.
        for ($attempt = 1; ; ++$attempt) {
            try {
                Copy-Item -LiteralPath $source_executable -Destination $installed_executable -Force
                break
            }
            catch {
                if ($attempt -ge 10) {
                    throw
                }
                Start-Sleep -Milliseconds 200
            }
        }
    }
    # 고지는 실행 파일 resource에 실렸다. 앞 판이 복사해 둔 라이선스 파일은 지운다.
    Remove-Item -LiteralPath (Join-Path $InstallDirectory 'json-LICENSE.txt') -Force -ErrorAction SilentlyContinue
}
$settings_directory = Split-Path -Parent $SettingsFile
New-Item -ItemType Directory -Path $settings_directory -Force | Out-Null
if (Test-Path -LiteralPath $SettingsFile) {
    $backup = $SettingsFile + '.session-board-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '.bak'
    Copy-Item -LiteralPath $SettingsFile -Destination $backup
    Write-Output "Backup: $backup"
}
$temporary = $SettingsFile + '.session-board.tmp'
[IO.File]::WriteAllText($temporary, $json_text, [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $temporary -Destination $SettingsFile -Force
Write-Output "Updated: $SettingsFile"
# 앱은 시작할 때 Hook을 자동 등록하므로 제거 의사를 표시 파일로 남긴다. 다시 설치하면 지운다.
$opt_out = Join-Path (Split-Path -Parent $InstallDirectory) 'claude-hooks.disabled'
if ($Remove) {
    New-Item -ItemType File -Path $opt_out -Force | Out-Null
    Write-Output "Automatic registration disabled: $opt_out"
}
elseif (Test-Path -LiteralPath $opt_out) {
    Remove-Item -LiteralPath $opt_out
}
if (-not $Remove) {
    Write-Output 'Claude Code picks up settings changes through its file watcher. Check the Session Board hooks with /hooks.'
}
