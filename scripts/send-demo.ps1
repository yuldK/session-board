#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$DataDirectory = (Join-Path $PSScriptRoot '../build/demo-sessions'),
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board-hook.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$DataDirectory = [IO.Path]::GetFullPath($DataDirectory)
$now_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$examples = @(
    @{ provider = 'codex'; session_id = 'demo-codex-build'; event = 'tool_started'; tool_name = 'exec_command'; tool_id = 'build'; title = 'Session Board · Release 빌드'; model = 'Codex'; summary = 'CMake Release 빌드 실행 중' },
    @{ provider = 'claude'; session_id = 'demo-claude-approval'; event = 'approval_requested'; tool_name = 'Bash'; title = 'Claude 어댑터 구현'; model = 'Claude Code'; summary = '테스트 명령 실행 승인을 기다립니다' },
    @{ provider = 'codex'; session_id = 'demo-codex-finished'; event = 'turn_finished'; title = '세션 상태 전이 테스트'; model = 'Codex'; summary = '상태 전이와 동시 저장 테스트 확인 완료' },
    @{ provider = 'claude'; session_id = 'demo-claude-stale'; event = 'tool_started'; tool_name = 'Bash'; title = '오래된 이벤트 표시 예제'; model = 'Claude Code'; timestamp_ms = $now_ms - 900000 },
    @{ provider = 'claude'; session_id = 'demo-claude-idle'; event = 'session_started'; title = '다음 요청을 기다리는 세션'; model = 'Claude Code' }
)
foreach ($example in $examples) {
    $example.schema_version = 1
    $example.cwd = (Split-Path -Parent $PSScriptRoot)
    if (-not $example.Contains('timestamp_ms')) {
        $example.timestamp_ms = $now_ms
    }
    $example | ConvertTo-Json -Compress | & $Executable --normalized --strict --data-dir $DataDirectory
    if ($LASTEXITCODE -ne 0) {
        throw 'Demo event failed.'
    }
}
Write-Output "Demo data: $DataDirectory"
Write-Output "Run: build/vs2026/bin/Release/session-board.exe --data-dir `"$DataDirectory`""
