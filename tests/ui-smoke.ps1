#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$UiaScript = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1'),
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [string]$HookExecutable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board-hook.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. $UiaScript
$project_root = Split-Path -Parent $PSScriptRoot
$data_directory = Join-Path $project_root ('build/ui-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $data_directory -Force | Out-Null
$app_path = (Resolve-Path -LiteralPath $Executable).Path

# 사람 화면의 포커스·커서를 건드리지 않도록 앱을 따로 만든 데스크톱에 띄운다.
# luil-uia의 격리 실행과 같되 실행 인자를 넘긴다 — 테스트 데이터 폴더를 써야 실제 설정과 Hook 등록을 건드리지 않는다.
function Connect-SessionBoardIsolated([string]$arguments) {
    if (Get-Process -Name 'session-board' -ErrorAction SilentlyContinue) {
        throw 'Session Board is already running. Close it before the isolated UI test.'
    }
    $script:luil_desktop_name = 'luil-uia-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $script:luil_desktop = [Luil.Uia.Native]::CreateDesktopW($script:luil_desktop_name, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($script:luil_desktop -eq [IntPtr]::Zero) {
        throw 'The isolated desktop could not be created.'
    }
    $script:luil_queue = Join-Path ([System.IO.Path]::GetTempPath()) "luil-uia\$($script:luil_desktop_name)"
    $null = New-Item -ItemType Directory -Path $script:luil_queue -Force
    $script:luil_isolated = $true
    $script:luil_step_id = 0
    $script:luil_root = $null
    $script:luil_process = Start-LuilProcess -path $app_path -arguments $arguments -desktop $script:luil_desktop_name
    $script:luil_started_app = $true
    $worker_path = (Get-Process -Id $PID).Path
    $worker_script = Join-Path (Split-Path -Parent $UiaScript) 'desktop_worker.ps1'
    $worker_arguments = '-NoProfile -STA -File "{0}" -queue "{1}" -process_id {2} -timeout_seconds 20' -f $worker_script, $script:luil_queue, $script:luil_process.Id
    $script:luil_worker = Start-LuilProcess -path $worker_path -arguments $worker_arguments -desktop $script:luil_desktop_name -no_window
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path (Join-Path $script:luil_queue 'ready.txt')) {
            return
        }
        if (Test-Path (Join-Path $script:luil_queue 'failed.txt')) {
            throw ('The isolated UIA worker failed: ' + (Get-Content (Join-Path $script:luil_queue 'failed.txt') -Raw))
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'The isolated UIA worker was not ready in time.'
}

# 격리 데스크톱 안에서 한 단계를 돈다. 요소는 데스크톱을 건널 수 없어 글로 보낸다.
function Step([string]$code) {
    return Invoke-LuilStep ([scriptblock]::Create($code))
}

function Send-Codex([string]$event_name, [string]$prompt = 'UI 실시간 갱신') {
    $payload = @{ hook_event_name = $event_name; session_id = 'uia-codex'; turn_id = 'uia-turn'; prompt = $prompt; tool_name = 'exec_command'; tool_use_id = 'uia-tool'; model = 'Codex'; cwd = $project_root }
    $payload | ConvertTo-Json -Compress | & $HookExecutable --provider codex --strict --data-dir $data_directory | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'UI test hook failed.'
    }
    if ($event_name -eq 'UserPromptSubmit') {
        $work = @{schema_version=1;provider='codex';session_id='uia-codex';turn_id='uia-turn';event='turn_started';timestamp_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();summary=$prompt}
        $work | ConvertTo-Json -Compress | & $HookExecutable --normalized --strict --data-dir $data_directory | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'UI work summary fixture failed.' }
    }
}

function Send-Claude([string]$event_name, [string]$session_id = 'uia-claude') {
    $title = if ($session_id -eq 'uia-claude') { 'Claude 공통 규격' } else { "스크롤 세션 $session_id" }
    $payload = @{ schema_version = 1; provider = 'claude'; session_id = $session_id; event = $event_name; timestamp_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); title = $title; summary = $title; cwd = $project_root }
    $payload | ConvertTo-Json -Compress | & $HookExecutable --normalized --strict --data-dir $data_directory | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'UI test normalized hook failed.'
    }
}

# 위젯 창의 모양과 크기 조절 손잡이의 판정을 격리 데스크톱 안에서 읽는다.
$window_probe = @'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SessionBoardWindow {
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr window, int index);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr word, IntPtr data);
}
"@ -ErrorAction SilentlyContinue
$handle = Find-LuilMainWindow -process_id (Get-Process -Name 'session-board').Id
$style = [SessionBoardWindow]::GetWindowLongPtrW($handle, -16).ToInt64()
$extended = [SessionBoardWindow]::GetWindowLongPtrW($handle, -20).ToInt64()
$hit = 0
$grip = Find-LuilElement -id 'widget-resize'
if ($null -ne $grip) {
    $bounds = $grip.Current.BoundingRectangle
    $point = [IntPtr]::new(([int]($bounds.Y + $bounds.Height / 2) -shl 16) -bor ([int]($bounds.X + $bounds.Width / 2) -band 65535))
    $hit = [SessionBoardWindow]::SendMessageW($handle, 0x84, [IntPtr]::Zero, $point).ToInt64()
}
'{0};{1};{2}' -f $style, $extended, $hit
'@

function Assert-WidgetWindow([bool]$topmost = $true) {
    $style, $extended, $hit = (Step $window_probe).Split(';') | ForEach-Object { [int64]$_ }
    # 테두리 없음, 작업 표시줄 없음, 색상 키 대신 DWM 합성이다.
    if (($style -band 0x00c00000) -ne 0 -or ($extended -band 0x80) -eq 0 -or ($extended -band 0x00080000) -ne 0) {
        throw 'The widget must be a borderless tool window composed by DWM, not a color-keyed layered window.'
    }
    if ((($extended -band 8) -ne 0) -ne $topmost) {
        throw "The widget topmost state should be $topmost."
    }
    if ($hit -ne 17) {
        throw 'The resize grip did not expose the native bottom-right resize action.'
    }
}

try {
    Connect-SessionBoardIsolated ('--data-dir "' + $data_directory + '"')
    # 위젯이 주 창이다. 상세 화면은 위젯에서 여는 독립 창이다.
    $null = Step "Wait-LuilElement -id 'widget-empty' -name '아직 수신한 세션이 없습니다'"
    Assert-WidgetWindow
    $null = Step "Wait-LuilElement -id 'filter-all' -all_surfaces -absent"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-detail')"
    $null = Step "Wait-LuilElement -id 'empty-title' -name '아직 수신한 세션이 없습니다' -all_surfaces"

    Send-Codex 'UserPromptSubmit'
    $null = Step "Wait-LuilElement -name 'UI 실시간 갱신' -all_surfaces"
    $codex_owner = (Get-ChildItem -LiteralPath $data_directory -Filter 'codex-*.json').BaseName
    Send-Codex 'PreToolUse'
    $null = Step "Wait-LuilElement -search '도구 실행 중 · exec_command' -all_surfaces"
    Send-Codex 'PermissionRequest'
    $null = Step "Wait-LuilElement -name '승인 대기' -all_surfaces"
    Send-Claude 'turn_started'
    $null = Step "Wait-LuilElement -name 'Claude 공통 규격' -all_surfaces"
    $null = Step "Select-LuilItem (Wait-LuilElement -id 'filter-claude' -all_surfaces)"
    $null = Step "Wait-LuilElement -id '$codex_owner-title' -all_surfaces -absent"
    $null = Step "Set-LuilToggle (Wait-LuilElement -id 'filter-working' -all_surfaces) -on `$true"
    Send-Claude 'turn_finished'
    $null = Step "Wait-LuilElement -id 'empty-title' -name '이 조건에 맞는 세션이 없습니다' -all_surfaces"
    $null = Step "Set-LuilToggle (Wait-LuilElement -id 'filter-working' -all_surfaces) -on `$false"
    $null = Step "Wait-LuilElement -name '응답 완료' -all_surfaces"
    $null = Step "Select-LuilItem (Wait-LuilElement -id 'filter-all' -all_surfaces)"
    Send-Codex 'PostToolUse'
    $null = Step "Wait-LuilElement -name '작업 중' -all_surfaces"
    Send-Codex 'Stop'
    $null = Step "Wait-LuilElement -id '$codex_owner-status' -name '응답 완료' -all_surfaces"

    # Hook이 세션 창을 기록했으면 이동 단추가 켜진다. 누르지는 않는다 — 다른 창을 앞으로 가져온다.
    $snapshot = Get-Content -LiteralPath (Join-Path $data_directory "$codex_owner.json") -Raw | ConvertFrom-Json
    $focus_enabled = Step "(Wait-LuilElement -id 'detail-focus' -all_surfaces).Current.IsEnabled"
    if ($snapshot.host_window -ne 0 -and "$focus_enabled" -notmatch 'True') {
        throw 'The focus button stayed disabled although the hook recorded the session window.'
    }

    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'theme' -all_surfaces)"
    $null = Step "Wait-LuilElement -id 'theme' -name '어둡게' -all_surfaces"
    $null = Step "Save-LuilShot -path '$(Join-Path $data_directory 'widget-light.png')'"
    $null = Step "Save-LuilShot -path '$(Join-Path $data_directory 'detail-light.png')' -surface (Get-LuilSurfaces | Where-Object { `$_.Current.Name -like '*상세*' })"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'theme' -all_surfaces)"
    $null = Step "Wait-LuilElement -id 'theme' -name '밝게' -all_surfaces"
    $null = Step "Save-LuilShot -path '$(Join-Path $data_directory 'detail-dark.png')' -surface (Get-LuilSurfaces | Where-Object { `$_.Current.Name -like '*상세*' })"

    # 위젯의 흐림을 끄고 진하기를 고른 뒤 설정 줄을 다시 접는다.
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')"
    $null = Step "Set-LuilToggle -on `$false (Wait-LuilElement -id 'widget-blur')"
    $blur = Step "(Get-LuilState (Wait-LuilElement -id 'widget-blur')).toggle"
    if ("$blur" -notmatch 'Off') {
        throw 'The blur switch did not turn off.'
    }
    $null = Step "Select-LuilItem (Wait-LuilElement -id 'opacity-50')"
    $null = Step "Save-LuilShot -path '$(Join-Path $data_directory 'widget-settings.png')'"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')"
    $null = Step "Wait-LuilElement -id 'widget-blur' -absent"

    $null = Step "Wait-LuilElement -id 'compact-$codex_owner' -search 'Codex.*응답 완료.*UI 실시간 갱신'"
    Send-Codex 'SessionEnd'
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner' -search 'Codex.*세션 종료.*UI 실시간 갱신'"
    Send-Codex 'UserPromptSubmit' '새 요청 내용이 갱신됩니다'
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner' -search '새 요청 내용이 갱신됩니다'"
    Send-Codex 'Stop'
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner-status' -name '응답 완료'"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'compact-$codex_owner')"
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner-identity-0'"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'compact-$codex_owner')"
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner-identity-0' -absent"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')"
    $null = Step "Wait-LuilElement -id 'widget-collapse' -name '목록 펼치기'"
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner' -absent"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')"
    $null = Step "Wait-LuilElement -id 'compact-$codex_owner'"
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-pin')"
    $null = Step "Wait-LuilElement -id 'widget-pin' -name '항상 위에 표시'"
    Assert-WidgetWindow -topmost $false
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-pin')"
    $null = Step "Wait-LuilElement -id 'widget-pin' -name '항상 위에 표시 해제'"
    Assert-WidgetWindow
    $null = Step "Save-LuilShot -path '$(Join-Path $data_directory 'widget-dark.png')'"

    # 상세 화면 목록이 넘치면 스크롤이 생긴다.
    for ($index = 1; $index -le 14; $index++) {
        Send-Claude 'turn_started' "extra-$index"
    }
    $null = Step "Wait-LuilElement -name '스크롤 세션 extra-14' -all_surfaces"
    $scrolled = Step @'
$find_bar = {
    Get-LuilSurfaces | Where-Object { $_.Current.Name -like '*상세*' } | ForEach-Object {
        $_.FindFirst([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::ScrollBar))
    } | Where-Object { $null -ne $_ } | Select-Object -First 1
}
$bar = & $find_bar
if ($null -eq $bar) { throw 'The detail list has no scroll bar.' }
$target = Set-LuilScroll $bar -end
$deadline = [DateTime]::UtcNow.AddSeconds(5)
while ($true) {
    $value = (& $find_bar).GetCurrentPattern([Windows.Automation.RangeValuePattern]::Pattern).Current.Value
    if ([Math]::Abs($value - $target) -lt 1) { 'scrolled'; break }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Scroll position was not applied.' }
    Start-Sleep -Milliseconds 100
}
'@
    if ("$scrolled" -notmatch 'scrolled') {
        throw 'Scroll position was not applied.'
    }

    # 위젯의 상세 화면 단추는 상세 창을 닫기도 한다.
    $null = Step "Invoke-LuilElement (Wait-LuilElement -id 'widget-detail')"
    $null = Step "Wait-LuilElement -id 'filter-all' -all_surfaces -absent"
    Write-Output 'UI smoke passed (isolated desktop): widget-first start, detail window, live updates, filters, themes, backdrop, end retention, expand/collapse, pin, resize grip, focus button state and scroll.'
    Get-ChildItem -LiteralPath $data_directory -Filter '*.png' | ForEach-Object FullName
}
finally {
    Disconnect-LuilApp
}
