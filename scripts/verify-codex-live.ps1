#requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$RunTurn,
    [ValidateRange(0, 300)][int]$HoldSeconds = 0,
    [int]$AppProcessId = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot
$info = [Diagnostics.ProcessStartInfo]::new((Get-Command codex -ErrorAction Stop).Source)
$info.ArgumentList.Add('app-server')
$info.ArgumentList.Add('--stdio')
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
$process = [Diagnostics.Process]::Start($info)
$trace = [Collections.Generic.List[object]]::new()
$observed_states = [Collections.Generic.HashSet[string]]::new()
$data_directory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'SessionBoard/sessions'
$session_id = ''
$snapshot_path = ''
$ui_proofs = [Collections.Generic.HashSet[string]]::new()
$verified_turn = $false
$ui_window_handle = [IntPtr]::Zero
if ($AppProcessId -gt 0) {
    . (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
    $ui_root = Connect-LuilApp -process_id $AppProcessId
    $ui_window_handle = [IntPtr]$ui_root.Current.NativeWindowHandle
}

function Confirm-LiveUi([string]$stage) {
    if ($AppProcessId -le 0 -or -not $snapshot_path -or $ui_proofs.Contains($stage)) {
        return
    }
    $owner = 'compact-' + [IO.Path]::GetFileNameWithoutExtension($snapshot_path)
    $card = Wait-LuilElement -id $owner
    if ($null -eq (Find-LuilElement -id ($owner + '-identity-0'))) {
        Invoke-LuilElement (Wait-LuilElement -id $owner)
    }
    $null = Wait-LuilElement -id ($owner + '-prompt-0') -search '실제 Codex 세션'
    switch ($stage) {
        'running' { $null = Wait-LuilElement -id ($owner + '-status') -name '작업 중' }
        'tool' { $null = Wait-LuilElement -id ($owner + '-activity') -search '도구 실행 중' }
        'completed' { $null = Wait-LuilElement -id ($owner + '-status') -name '응답 완료' }
        'ended' {
            $null = Wait-LuilElement -id ($owner + '-status') -name '세션 종료'
            $null = Wait-LuilElement -id ($owner + '-identity-0') -search $session_id
        }
    }
    $card = Wait-LuilElement -id $owner
    $bounds = $card.Current.BoundingRectangle
    if ($bounds.IsEmpty -or $bounds.Width -lt 250 -or $bounds.Height -lt 50) {
        throw "The real session was present in the tree but not visibly laid out: $stage"
    }
    Save-LuilShot -path (Join-Path $project_root "build/live-widget-$stage.png") | Out-Null
    $null = $ui_proofs.Add($stage)
    Write-Output "UI confirmed: $stage, $session_id"
}

function Write-LiveProof {
    @{ session_id = $session_id; snapshot_path = $snapshot_path; states = @($observed_states); ui_proofs = @($ui_proofs); trace = @($trace.ToArray()) } |
        ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $project_root 'build/live-codex-verification.json') -Encoding utf8NoBOM
}

function Find-Snapshot {
    if (-not $script:session_id -or -not (Test-Path -LiteralPath $data_directory)) {
        return
    }
    if (-not $script:snapshot_path) {
        foreach ($file in Get-ChildItem -LiteralPath $data_directory -Filter 'codex-*.json') {
            $snapshot = Get-Content -LiteralPath $file.FullName -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
            if ($snapshot.session_id -eq $script:session_id) {
                $script:snapshot_path = $file.FullName
                break
            }
        }
    }
}

function Read-Message {
    $read = $process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait(30000) -or $null -eq $read.Result) {
        throw 'Codex protocol response timed out or closed.'
    }
    $message = $read.Result | ConvertFrom-Json -AsHashtable
    if ($message.Contains('method') -and $message.method -in @('hook/completed', 'turn/started', 'turn/completed')) {
        $trace.Add($message)
        Find-Snapshot
        if ($script:snapshot_path -and (Test-Path -LiteralPath $script:snapshot_path)) {
            $snapshot = Get-Content -LiteralPath $script:snapshot_path -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
            $null = $observed_states.Add($snapshot.status)
            if ($AppProcessId -gt 0 -and $message.method -eq 'hook/completed' -and $message.params.run.status -eq 'completed') {
                if ($message.params.run.eventName -eq 'userPromptSubmit') {
                    Confirm-LiveUi 'running' | Out-Host
                }
                elseif ($message.params.run.eventName -eq 'preToolUse') {
                    Confirm-LiveUi 'tool' | Out-Host
                }
            }
        }
    }
    return $message
}

function Send-Rpc([int]$rpc_id, [string]$method, [hashtable]$parameters) {
    $request = @{ id = $rpc_id; method = $method; params = $parameters } | ConvertTo-Json -Depth 20 -Compress
    $process.StandardInput.WriteLine($request)
    $process.StandardInput.Flush()
    do {
        $reply = Read-Message
    } while (-not ($reply.Contains('id') -and $reply.id -eq $rpc_id))
    if ($reply.Contains('error')) {
        throw ($reply.error | ConvertTo-Json -Compress)
    }
    return $reply.result
}

function Get-OwnHooks($node) {
    if ($node -is [Collections.IDictionary]) {
        if ($node.Contains('trustStatus') -and (($node | ConvertTo-Json -Depth 16 -Compress) -match 'session-board-hook')) {
            Write-Output $node
        }
        foreach ($value in $node.Values) {
            Get-OwnHooks $value
        }
    }
    elseif ($node -is [Collections.IEnumerable] -and $node -isnot [string]) {
        foreach ($value in $node) {
            Get-OwnHooks $value
        }
    }
}

try {
    $null = Send-Rpc 1 'initialize' @{ clientInfo = @{ name = 'session_board_verify'; title = 'Session Board Verify'; version = '0.1.0' }; capabilities = @{ experimentalApi = $true } }
    $process.StandardInput.WriteLine('{"method":"initialized"}')
    $hooks = Send-Rpc 2 'hooks/list' @{ cwds = @($project_root) }
    $hooks | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath (Join-Path $project_root 'build/codex-hooks-status.json') -Encoding utf8NoBOM
    $own_hooks = @(Get-OwnHooks $hooks)
    if ($RunTurn -and ($own_hooks.Count -ne 8 -or @($own_hooks | Where-Object { -not $_.enabled -or $_.trustStatus -ne 'trusted' }).Count -gt 0)) {
        throw 'Review and trust all eight Session Board hooks before running the live verification.'
    }
    if (-not $RunTurn) {
        $own_hooks | ForEach-Object { [PSCustomObject]@{ Event = $_.eventName; Trust = $_.trustStatus; Enabled = $_.enabled } }
        return
    }
    $started = Send-Rpc 3 'thread/start' @{ cwd = $project_root; ephemeral = $true; sandbox = 'read-only'; approvalPolicy = 'never' }
    $session_id = $started.thread.id
    Write-Output "Actual Codex session: $session_id"
    if ($RunTurn) {
        $text = '실제 Codex 세션: 투명 목록 UI 검증. 파일을 변경하지 마세요. 셸 도구로 현재 작업 폴더를 한 번 출력하고 3초 기다리세요. 마지막에 연동 확인 완료라고 답하세요.'
        $null = Send-Rpc 4 'turn/start' @{ threadId = $session_id; input = @(@{ type = 'text'; text = $text }) }
        do {
            $message = Read-Message
        } while (-not ($message.Contains('method') -and $message.method -eq 'turn/completed'))
        if ($message.params.turn.status -ne 'completed') {
            throw "The verification turn did not complete: $($message.params.turn.status)"
        }
        Find-Snapshot
        if (-not $snapshot_path) {
            throw 'The actual hooks did not create a snapshot. See build/codex-hook-failure.json.'
        }
        $snapshot = Get-Content -LiteralPath $snapshot_path -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
        if ($snapshot.status -ne 'idle' -or -not $snapshot.turn_closed) {
            throw 'The actual Stop hook did not report a finished response.'
        }
        $null = $observed_states.Add($snapshot.status)
        Confirm-LiveUi 'completed'
        $verified_turn = $true
        Write-Output "Actual turn completed. Observed states: $($observed_states -join ', ')"
        Write-Output "Snapshot: $snapshot_path"
    }
    Write-LiveProof
    $hold_until = [DateTime]::UtcNow.AddSeconds($HoldSeconds)
    while ([DateTime]::UtcNow -lt $hold_until) {
        Start-Sleep -Milliseconds 250
    }
}
catch {
    $trace | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $project_root 'build/codex-hook-failure.json') -Encoding utf8NoBOM
    throw
}
finally {
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(5000)) {
        $process.Kill($true)
    }
    $process.Dispose()
    try {
        if ($verified_turn -and $AppProcessId -gt 0) {
            Confirm-LiveUi 'ended'
            Write-LiveProof
        }
    }
    finally {
        if ($AppProcessId -gt 0) {
            Disconnect-LuilApp
            # 검증 결과를 사람이 볼 수 있도록 포커스 복구 도우미의 최소화만 되돌린다.
            $null = [Luil.Uia.Native]::ShowWindow($ui_window_handle, 4)
        }
    }
}
