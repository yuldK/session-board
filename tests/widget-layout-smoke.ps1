#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [string]$UiaScript = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. $UiaScript
. (Join-Path $PSScriptRoot 'isolated-uia.ps1')
$app_path = (Resolve-Path -LiteralPath $Executable).Path
$work_directory = Join-Path (Split-Path -Parent $PSScriptRoot) ('build/widget-layout-' + [guid]::NewGuid().ToString('N'))
$data_directory = Join-Path $work_directory 'sessions'
$null = New-Item -ItemType Directory -Path $data_directory -Force
@{
    schema_version = 1; provider = 'codex'; session_id = 'layout-test'; event = 'turn_started'
    timestamp_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); summary = '첫 번째 작업'; cwd = $work_directory
} | ConvertTo-Json -Compress | & $app_path --hook --normalized --strict --data-dir $data_directory | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Test hook failed.' }

try {
    Connect-SessionBoardIsolated -Executable $app_path -UiaScript $UiaScript -Arguments ('--data-dir "{0}" --position=80,80' -f $data_directory)
    # 실행 경로와 인자는 PowerShell 단일 인용 문자열로 전달한다. UIA 요소는 데스크톱을 건너지 않는다.
    $worker_test = Join-Path $PSScriptRoot 'widget-layout-checks.ps1'
    $command = ". '{0}' -Executable '{1}' -WorkDirectory '{2}' -AppProcessId {3}" -f $worker_test.Replace("'", "''"), $app_path.Replace("'", "''"), $work_directory.Replace("'", "''"), $script:luil_process.Id
    Invoke-LuilStep ([scriptblock]::Create($command))
}
finally {
    Disconnect-LuilApp
}
