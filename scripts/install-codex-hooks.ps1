#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'SessionBoard/bin'),
    [string]$HooksFile,
    [switch]$Preview,
    [switch]$Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $HooksFile) {
    $codex_directory = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $env:USERPROFILE '.codex' }
    $HooksFile = Join-Path $codex_directory 'hooks.json'
}
$HooksFile = [IO.Path]::GetFullPath($HooksFile)
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$installed_executable = Join-Path $InstallDirectory 'session-board.exe'
$opt_out = Join-Path (Split-Path -Parent $InstallDirectory) 'codex-hooks.disabled'
$document = if (Test-Path -LiteralPath $HooksFile) { Get-Content -LiteralPath $HooksFile -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable } else { @{} }
if (-not $document.Contains('hooks')) {
    $document.hooks = @{}
}
$events = @('SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse', 'PermissionRequest', 'Stop', 'Interrupt', 'SessionEnd')
foreach ($event_name in $events) {
    $groups = [Collections.Generic.List[object]]::new()
    if ($document.hooks.Contains($event_name)) {
        foreach ($group in @($document.hooks[$event_name])) {
            $handlers = @($group.hooks | Where-Object { -not ($_.Contains('command') -and $_.command -match 'session-board(-hook)?\.exe"?\s+(--hook\s+)?--provider codex$') })
            if ($handlers.Count -gt 0) {
                $group.hooks = $handlers
                $groups.Add($group)
            }
        }
    }
    if (-not $Remove) {
        $command = '"' + $installed_executable.Replace('\', '/') + '" --hook --provider codex'
        $command_windows = "& '" + $installed_executable.Replace('\', '/').Replace("'", "''") + "' --hook --provider codex"
        $groups.Add(@{ hooks = @(@{ type = 'command'; command = $command; commandWindows = $command_windows; timeout = 2 }) })
    }
    if ($groups.Count -gt 0) {
        $document.hooks[$event_name] = @($groups.ToArray())
    }
    else {
        $document.hooks.Remove($event_name)
    }
}
$json_text = ($document | ConvertTo-Json -Depth 50).Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n"
if ($Preview) {
    Write-Output $json_text
    return
}
if (-not $Remove) {
    $source_executable = (Resolve-Path -LiteralPath $Executable).Path
    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    if ($source_executable -ne $installed_executable) {
        Copy-Item -LiteralPath $source_executable -Destination $installed_executable -Force
    }
    # 고지는 실행 파일 resource에 실렸다. 앞 판이 복사해 둔 라이선스 파일은 지운다.
    Remove-Item -LiteralPath (Join-Path $InstallDirectory 'json-LICENSE.txt') -Force -ErrorAction SilentlyContinue
}
if ($Remove) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $opt_out) -Force | Out-Null
    [IO.File]::WriteAllText($opt_out, '', [Text.UTF8Encoding]::new($false))
}
elseif (Test-Path -LiteralPath $opt_out) {
    Remove-Item -LiteralPath $opt_out
}
$hooks_directory = Split-Path -Parent $HooksFile
New-Item -ItemType Directory -Path $hooks_directory -Force | Out-Null
if (Test-Path -LiteralPath $HooksFile) {
    $backup = $HooksFile + '.session-board-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '.bak'
    Copy-Item -LiteralPath $HooksFile -Destination $backup
    Write-Output "Backup: $backup"
}
$temporary = $HooksFile + '.session-board.tmp'
[IO.File]::WriteAllText($temporary, $json_text, [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $temporary -Destination $HooksFile -Force
Write-Output "Updated: $HooksFile"
if (-not $Remove) {
    Write-Output 'Review and trust the Session Board hooks in Codex (/hooks), then start a new chat or resume a session.'
}
