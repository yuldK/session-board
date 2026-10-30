#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$WorkDirectory,
    [Parameter(Mandatory)][int]$AppProcessId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
namespace SessionBoardLayoutTest {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Monitor { public uint size; public Rect monitor, work; public uint flags; }
    public static class Native {
        [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
        [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect value);
        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
        [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr monitor, ref Monitor value);
        [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr word, IntPtr data);
        [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
        [DllImport("user32.dll")] public static extern IntPtr GetThreadDesktop(uint thread);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetUserObjectInformationW(IntPtr handle, int index, StringBuilder value, int size, out uint needed);
    }
}
'@
$desktop_name = [Text.StringBuilder]::new(256)
$needed = [uint32]0
$desktop = [SessionBoardLayoutTest.Native]::GetThreadDesktop([SessionBoardLayoutTest.Native]::GetCurrentThreadId())
if (-not [SessionBoardLayoutTest.Native]::GetUserObjectInformationW($desktop, 2, $desktop_name, 512, [ref]$needed) -or -not $desktop_name.ToString().StartsWith('luil-uia-')) {
    throw 'Widget layout checks require the luil-uia isolated desktop.'
}
$null = [SessionBoardLayoutTest.Native]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))
$null = [SessionBoardLayoutTest.Native]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
$Executable = [IO.Path]::GetFullPath($Executable)
$work_directory = $WorkDirectory
$data_directory = Join-Path $work_directory 'sessions'
$null = New-Item -ItemType Directory -Path $data_directory -Force
$script:checks = 0

function Assert-Layout([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { throw $message }
}

function Send-Update([string]$summary, [string]$session_id = 'layout-test', [string]$event_name = 'turn_started') {
    @{
        schema_version = 1; provider = 'codex'; session_id = $session_id; event = $event_name
        timestamp_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); summary = $summary; cwd = $work_directory
    } | ConvertTo-Json -Compress | & $Executable --hook --normalized --strict --data-dir $data_directory | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Test hook failed.' }
}

function Get-Box {
    $box = [SessionBoardLayoutTest.Rect]::new()
    $null = [SessionBoardLayoutTest.Native]::GetWindowRect($script:window_handle, [ref]$box)
    return $box
}

function Get-NativeHit([int]$x, [int]$y) {
    return [SessionBoardLayoutTest.Native]::SendMessageW($script:window_handle, 0x84, [IntPtr]::Zero, [IntPtr]::new(($y -shl 16) -bor ($x -band 65535))).ToInt64()
}

function Assert-CaptionHit([int]$header_y) {
    $title = (Wait-LuilElement -id 'widget-title').Current.BoundingRectangle
    Assert-Layout ((Get-NativeHit ([int]($title.Right - 8)) ([int]($header_y + $title.Height / 2))) -eq 2) 'luil did not expose HTCAPTION at the drawn header.'
    foreach ($id in 'widget-detail', 'widget-theme', 'widget-backdrop', 'widget-pin', 'widget-collapse', 'compact-close') {
        $button = (Wait-LuilElement -id $id).Current.BoundingRectangle
        Assert-Layout ((Get-NativeHit ([int]($button.X + $button.Width / 2)) ([int]($button.Y + $button.Height / 2))) -eq 1) "The caption button $id became a drag or native window-button target."
    }
}

function Move-Widget([int]$x, [int]$y, [int]$width, [int]$height) {
    $null = [SessionBoardLayoutTest.Native]::SetWindowPos($script:window_handle, [IntPtr]::Zero, $x, $y, $width, $height, 0x14)
}

function Wait-Header([double]$y) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $header = (Wait-LuilElement -id 'widget-title').Current.BoundingRectangle
        if ([Math]::Abs($header.Y - $y) -le 1) { return $header }
        Start-Sleep -Milliseconds 30
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Header moved: expected $y, actual $($header.Y)"
}

function Get-UpdateSurface {
    return Get-PopupSurface 'update-count'
}

function Get-PopupSurface([string]$id) {
    $element = Wait-LuilElement -id $id -all_surfaces
    $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
    while ($null -ne $element) {
        $handle = $element.Current.NativeWindowHandle
        if ($handle -ne 0 -and $handle -ne $script:window_handle) { return $element }
        $element = $walker.GetParent($element)
    }
    throw 'The notification has no native popup surface.'
}

function Toggle-StableHeader([double]$y, [bool]$collapsed) {
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $header = (Wait-LuilElement -id 'widget-title').Current.BoundingRectangle
        Assert-Layout ([Math]::Abs($header.Y - $y) -le 1) "Header jumped during collapse transition: expected $y, actual $($header.Y)"
        $signal = Find-LuilElement -id 'widget-signal'
        $box = Get-Box
        if ($collapsed -and $null -eq $signal -and $box.bottom - $box.top -eq $caption) { return }
        if (-not $collapsed -and $null -ne $signal -and $box.bottom - $box.top -gt $caption) { return }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Collapse transition did not complete.'
}


try {
    Send-Update '첫 번째 작업'
    $ui_root = Connect-LuilApp -process_id $AppProcessId
    $script:window_handle = [IntPtr]$ui_root.Current.NativeWindowHandle
    $monitor = [SessionBoardLayoutTest.Monitor]::new()
    $monitor.size = [Runtime.InteropServices.Marshal]::SizeOf($monitor)
    $null = [SessionBoardLayoutTest.Native]::GetMonitorInfoW([SessionBoardLayoutTest.Native]::MonitorFromWindow($window_handle, 2), [ref]$monitor)
    $work = $monitor.work
    $scale = [SessionBoardLayoutTest.Native]::GetDpiForWindow($window_handle) / 96.0
    $caption = [int](40 * $scale)
    $width = [int](400 * $scale)
    $height = [int](560 * $scale)
    $x = $work.left + 50
    $top_y = $work.top + 50
    Move-Widget $x $top_y $width $height
    $null = Wait-Header $top_y
    Assert-CaptionHit $top_y
    # 카드 진하기를 5%까지 내려도 카드 글에는 후광·테가 서서 읽힌다. 값은 다시 50%로 둔다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $card = Wait-LuilElement -id 'widget-card-opacity' -all_surfaces
    Assert-Layout $card.Current.IsEnabled 'Card opacity was not adjustable over the default Acrylic backdrop.'
    $null = Set-LuilScroll -fraction 0.05 $card
    $null = Wait-LuilElement -id 'widget-card-opacity-value' -name '5%' -all_surfaces
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'widget-card-opacity' -all_surfaces -absent
    Save-LuilShot -path (Join-Path $work_directory 'cards-faint.png')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Set-LuilScroll -fraction 0.5 (Wait-LuilElement -id 'widget-card-opacity' -all_surfaces)
    $null = Wait-LuilElement -id 'widget-card-opacity-value' -name '50%' -all_surfaces
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'widget-card-opacity' -all_surfaces -absent
    Save-LuilShot -path (Join-Path $work_directory 'cards-half.png')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal' -absent
    $null = Wait-Header $top_y
    Assert-Layout (((Get-Box).bottom - (Get-Box).top) -eq $caption) 'Collapsed widget did not keep only the header.'
    # 목록을 접어 둔 채로도 테마와 배경 설정을 고른다. 판은 머릿줄 아래에 뜨는 팝업이고 한 번에 하나만 뜬다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-theme')
    $settings = (Get-PopupSurface 'theme-mode').Current.BoundingRectangle
    Assert-Layout ($settings.Top -ge (Get-Box).bottom -and $settings.Bottom -le $work.bottom) 'Theme settings did not open below the collapsed header.'
    Assert-Layout (((Get-Box).bottom - (Get-Box).top) -eq $caption) 'Opening theme settings expanded the collapsed widget.'
    Save-LuilShot -path (Join-Path $work_directory 'settings-theme-collapsed.png') -surface (Get-PopupSurface 'theme-mode')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'widget-opacity' -all_surfaces
    Assert-Layout ($null -eq (Find-LuilElement -id 'theme-mode' -all_surfaces)) 'Two settings panels were open at once.'
    Save-LuilShot -path (Join-Path $work_directory 'settings-backdrop-collapsed.png') -surface (Get-PopupSurface 'widget-opacity')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'widget-opacity' -all_surfaces -absent
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal'
    $null = Wait-Header $top_y

    $bottom_y = $work.bottom - $caption - 50
    Move-Widget $x $bottom_y $width $height
    $null = Wait-Header $bottom_y
    $box = Get-Box
    Assert-Layout ($box.top -lt $bottom_y -and $box.bottom -eq $bottom_y + $caption) 'Bottom overflow did not place the list above the fixed header.'
    Assert-CaptionHit $bottom_y
    Assert-Layout ((Get-NativeHit ($box.left + 100) ($box.top + 24)) -eq 1) 'The old top caption position still exposes a drag or window-button target.'
    Save-LuilShot -path (Join-Path $work_directory 'header-bottom.png')
    # 배치 좌표만 본다. 창 DC의 픽셀은 DWM이 합성한 화면과 달라 전환 중 튐을 보지 못하므로 화면은 tests/widget-header-flash.ps1이 실제 데스크톱에서 잰다.
    foreach ($iteration in 1..40) {
        Toggle-StableHeader $bottom_y $true
        Toggle-StableHeader $bottom_y $false
    }
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal' -absent
    $null = Wait-Header $bottom_y
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal'
    $null = Wait-Header $bottom_y

    Move-Widget $x ($top_y + $caption - $height) $width $height
    $null = Wait-Header $top_y
    Assert-Layout ((Get-Box).top -eq $top_y) 'Top overflow did not place the list below the fixed header.'
    Save-LuilShot -path (Join-Path $work_directory 'header-top.png')
    # 머릿줄 위치를 아래로 정하면 창 사각형은 그대로 두고 머릿줄만 바닥으로 옮긴다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    Assert-Layout ((Get-LuilState (Wait-LuilElement -id 'header-automatic' -all_surfaces)).selected) 'Header placement did not start as automatic.'
    Select-LuilItem (Wait-LuilElement -id 'header-bottom' -all_surfaces)
    $null = Wait-Header ($top_y + $height - $caption)
    $box = Get-Box
    Assert-Layout ($box.top -eq $top_y -and $box.bottom -eq $top_y + $height) 'Choosing a bottom header moved the widget.'
    Assert-Layout ((Get-LuilState (Wait-LuilElement -id 'header-bottom' -all_surfaces)).selected) 'The bottom header choice was not selected.'
    Save-LuilShot -path (Join-Path $work_directory 'settings-header-placement.png') -surface (Get-PopupSurface 'header-bottom')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'header-bottom' -all_surfaces -absent
    # 정해 둔 아래 머릿줄은 화면 위 끝에 닿아도 뒤집지 않는다. 목록이 창 최소 높이를 얻을 만큼만 화면 끝에서 떨어진다.
    #  접고 펼쳐도 머릿줄은 제자리다.
    $minimum = [int](180 * $scale)
    Move-Widget $x ($top_y + $caption - $height) $width $height
    $fixed_y = $work.top + $minimum - $caption
    $null = Wait-Header $fixed_y
    $box = Get-Box
    Assert-Layout ($box.top -eq $work.top -and $box.bottom -eq $fixed_y + $caption) 'A fixed bottom header flipped or squeezed the list at the top edge.'
    Save-LuilShot -path (Join-Path $work_directory 'header-fixed-bottom.png')
    Toggle-StableHeader $fixed_y $true
    Toggle-StableHeader $fixed_y $false
    # 위로 정하면 같은 사각형의 위 끝으로 머릿줄을 옮기고, 화면 아래 끝에 닿아도 뒤집지 않는다.
    Move-Widget $x $top_y $width $height
    $null = Wait-Header ($top_y + $height - $caption)
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    Select-LuilItem (Wait-LuilElement -id 'header-top' -all_surfaces)
    $null = Wait-Header $top_y
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'header-top' -all_surfaces -absent
    Assert-Layout ((Get-Box).top -eq $top_y -and (Get-Box).bottom -eq $top_y + $height) 'Choosing a top header moved the widget.'
    Move-Widget $x $bottom_y $width $height
    $fixed_y = $work.bottom - $minimum
    $null = Wait-Header $fixed_y
    Assert-Layout ((Get-Box).top -eq $fixed_y -and (Get-Box).bottom -eq $work.bottom) 'A fixed top header flipped or squeezed the list at the bottom edge.'
    Save-LuilShot -path (Join-Path $work_directory 'header-fixed-top.png')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    Select-LuilItem (Wait-LuilElement -id 'header-automatic' -all_surfaces)
    Invoke-LuilElement (Wait-LuilElement -id 'widget-backdrop')
    $null = Wait-LuilElement -id 'header-automatic' -all_surfaces -absent
    Move-Widget $x $top_y $width $height
    $null = Wait-Header $top_y
    Move-Widget $x $top_y (($work.right - $work.left) * 3) (($work.bottom - $work.top) * 3)
    $box = Get-Box
    Assert-Layout ($box.left -ge $work.left -and $box.top -ge $work.top -and $box.right -le $work.right -and $box.bottom -le $work.bottom) 'Oversized widget escaped the monitor work area.'
    Move-Widget $x $top_y $width $height
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal' -absent
    Send-Update '동작 중 알림 금지' 'running-notice'
    $null = Wait-LuilElement -id 'widget-count' -name '확인 0 · 진행 2 · 응답 0 · 새 알림 0'
    Assert-Layout ($null -eq (Find-LuilElement -id 'update-count' -all_surfaces)) 'A newly running session generated a popup.'
    # 작업 중이던 세션이 끝나면 무엇이 끝났는지 놓치지 않게 닫을 때까지 남는 알림을 띄운다.
    Send-Update '작업 완료 알림' 'running-notice' 'turn_finished'
    $null = Wait-LuilElement -id 'widget-count' -name '확인 0 · 진행 1 · 응답 1 · 새 알림 1'
    $null = Wait-LuilElement -id 'update-count' -name '작업 끝남 1개' -all_surfaces
    $null = Wait-LuilElement -name '작업 완료 알림' -all_surfaces
    $done_button = Wait-LuilElement -search 'update-.*-confirm' -name '닫기' -all_surfaces
    Assert-Layout $done_button.Current.IsEnabled 'A finished-work notification cannot be dismissed.'
    Save-LuilShot -path (Join-Path $work_directory 'finished-notification.png') -surface (Get-UpdateSurface)
    Invoke-LuilElement $done_button
    $null = Wait-LuilElement -id 'update-count' -all_surfaces -absent

    Send-Update '승인 응답을 기다립니다' 'waiting-notice' 'approval_requested'
    $null = Wait-LuilElement -name '승인 응답을 기다립니다' -all_surfaces
    $popup = Get-UpdateSurface
    $popup_box = $popup.Current.BoundingRectangle
    Assert-Layout ($popup_box.Top -ge (Get-Box).bottom -and $popup_box.Bottom -le $work.bottom) 'Response notification was not below the top header or escaped the work area.'
    Save-LuilShot -path (Join-Path $work_directory 'notification-below.png') -surface $popup
    $waiting_button = Wait-LuilElement -search 'update-.*-confirm' -name '응답 대기' -all_surfaces
    # 목록을 접은 채로도 알린 세션의 창으로 갈 수 있다. 창 정보가 없는 세션이면 단추가 이유를 말하며 꺼진다.
    $focus_button = Wait-LuilElement -search 'update-.*-focus' -all_surfaces
    Assert-Layout ($focus_button.Current.Name -match '세션 창으로 이동|창 정보가 없습니다') 'Notification card has no session focus button.'
    Assert-Layout (-not $waiting_button.Current.IsEnabled) 'An outstanding response notification can be dismissed.'
    Send-Update '알림을 만들지 않는 진행 메시지' 'ordinary-notice'
    $null = Wait-LuilElement -id 'widget-count' -name '확인 1 · 진행 2 · 응답 1 · 새 알림 1'
    $null = Wait-LuilElement -id 'update-count' -name '확인 필요 1개' -all_surfaces
    Assert-Layout ($null -eq (Find-LuilElement -name '알림을 만들지 않는 진행 메시지' -all_surfaces)) 'Ordinary progress appeared alongside the response request.'
    # 표식 넷이 다 서도 기본 폭의 머릿줄은 단추를 모으지 않는다. 모을지는 수가 아니라 폭으로만 정한다.
    Assert-Layout ($null -ne (Find-LuilElement -id 'widget-pin') -and $null -eq (Find-LuilElement -id 'widget-more')) 'Default-width header gathered its buttons when every count was shown.'
    Save-LuilShot -path (Join-Path $work_directory 'header-counts.png')
    # 상태 표식은 머무르거나 누르면 표식마다 센 세션을 팝업으로 보인다. 다시 누르면 닫힌다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-count')
    $status = Get-PopupSurface 'status-attention-name'
    Assert-Layout ($status.Current.BoundingRectangle.Top -ge (Get-Box).bottom) 'Status details did not open below the header.'
    $null = Wait-LuilElement -id 'status-running-name' -all_surfaces
    $null = Wait-LuilElement -id 'status-done-name' -all_surfaces
    # 응답을 기다리는 알림은 확인 필요 묶음이 이미 보이므로 새 알림 묶음에 다시 세우지 않는다.
    Assert-Layout ($null -eq (Find-LuilElement -id 'status-alerts-name' -all_surfaces)) 'A waiting session appeared twice in the status details.'
    Assert-Layout ($null -eq (Find-LuilElement -id 'update-count' -all_surfaces)) 'Notifications overlapped the status details.'
    Save-LuilShot -path (Join-Path $work_directory 'status-details.png') -surface $status
    Invoke-LuilElement (Wait-LuilElement -id 'widget-count')
    $null = Wait-LuilElement -id 'status-attention-name' -all_surfaces -absent
    $null = Wait-LuilElement -id 'update-count' -all_surfaces

    # 가장 좁은 위젯의 머릿줄은 접기·닫기만 남기고 나머지 단추를 하나로 모아 팝업으로 펼친다.
    Move-Widget $x $top_y ([int](320 * $scale)) $caption
    $null = Wait-LuilElement -id 'widget-more'
    Assert-Layout ($null -eq (Find-LuilElement -id 'widget-pin')) 'Narrow header kept every button.'
    $null = Wait-LuilElement -id 'widget-collapse'
    $null = Wait-LuilElement -id 'compact-close'
    Save-LuilShot -path (Join-Path $work_directory 'header-narrow.png')
    Invoke-LuilElement (Wait-LuilElement -id 'widget-more')
    $actions = (Get-PopupSurface 'widget-pin').Current.BoundingRectangle
    $more = (Wait-LuilElement -id 'widget-more').Current.BoundingRectangle
    Assert-Layout ($actions.Top -ge $more.Bottom -and [Math]::Abs($actions.Right - $more.Right) -le [Math]::Ceiling(8 * $scale)) 'Gathered buttons did not open under their button.'
    Assert-Layout ($null -eq (Find-LuilElement -id 'update-count' -all_surfaces)) 'Notifications overlapped the gathered buttons.'
    Save-LuilShot -path (Join-Path $work_directory 'header-gathered.png') -surface (Get-PopupSurface 'widget-pin')
    # 모은 단추에서 연 설정 판은 모은 단추 팝업을 대신한다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-theme' -all_surfaces)
    $null = Wait-LuilElement -id 'theme-mode' -all_surfaces
    Assert-Layout ($null -eq (Find-LuilElement -id 'widget-pin' -all_surfaces)) 'Gathered buttons stayed open beside the settings panel.'
    Invoke-LuilElement (Wait-LuilElement -id 'widget-more')
    $null = Wait-LuilElement -id 'widget-pin' -all_surfaces
    Invoke-LuilElement (Wait-LuilElement -id 'widget-more')
    $null = Wait-LuilElement -id 'widget-pin' -all_surfaces -absent
    $null = Wait-LuilElement -id 'update-count' -all_surfaces
    Move-Widget $x $top_y $width $caption
    $null = Wait-LuilElement -id 'widget-pin'

    Move-Widget $x $bottom_y $width $caption
    $null = Wait-Header $bottom_y
    $popup = Get-UpdateSurface
    $popup_box = $popup.Current.BoundingRectangle
    Assert-Layout ($popup_box.Bottom -le (Get-Box).top -and $popup_box.Top -ge $work.top) 'Response notification was not above the bottom header or escaped the work area.'
    Save-LuilShot -path (Join-Path $work_directory 'notification-above.png') -surface $popup
    # 바닥 머릿줄에서는 설정 판도 위로 뜬다. 판이 떠 있는 동안 알림 판은 숨었다가 닫으면 돌아온다.
    Invoke-LuilElement (Wait-LuilElement -id 'widget-theme')
    $settings = (Get-PopupSurface 'theme-mode').Current.BoundingRectangle
    Assert-Layout ($settings.Bottom -le (Get-Box).top -and $settings.Top -ge $work.top) 'Theme settings were not above the bottom header or escaped the work area.'
    $null = Wait-LuilElement -id 'update-count' -all_surfaces -absent
    Invoke-LuilElement (Wait-LuilElement -id 'widget-theme')
    $null = Wait-LuilElement -id 'theme-mode' -all_surfaces -absent
    $null = Wait-LuilElement -id 'update-count' -all_surfaces
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'widget-signal'
    $null = Wait-LuilElement -id 'update-count' -all_surfaces -absent
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -name '응답 대기' -all_surfaces
    Assert-Layout $true 'Outstanding response did not return after reopening and collapsing.'
    Save-LuilShot -path (Join-Path $work_directory 'persistent-response.png') -surface (Get-UpdateSurface)
    $persistent_until = [DateTime]::UtcNow.AddSeconds(9)
    while ([DateTime]::UtcNow -lt $persistent_until) {
        $response = Wait-LuilElement -name '응답 대기' -all_surfaces
        Assert-Layout (-not $response.Current.IsEnabled) 'An outstanding request expired or became dismissible.'
        Start-Sleep -Milliseconds 100
    }
    Send-Update '승인 후 작업 중 메시지' 'waiting-notice'
    $null = Wait-LuilElement -name '응답 처리됨' -all_surfaces
    Assert-Layout ($null -eq (Find-LuilElement -name '작업 중' -all_surfaces)) 'Resolved response was shown as a running notification.'
    Assert-Layout ($null -eq (Find-LuilElement -name '승인 후 작업 중 메시지' -all_surfaces)) 'Running progress replaced the resolved request.'
    $resolved_at = [DateTime]::UtcNow
    $null = Wait-LuilElement -id 'update-count' -all_surfaces -absent -timeout_seconds 12
    Assert-Layout (([DateTime]::UtcNow - $resolved_at).TotalSeconds -ge 7) 'Externally resolved response notification vanished too soon.'

    foreach ($index in 1..5) {
        Send-Update "입력이 필요한 항목 $index" "multiple-$index" 'input_requested'
    }
    $null = Wait-LuilElement -id 'update-count' -name '확인 필요 5개' -all_surfaces
    $null = Set-LuilScroll (Wait-LuilElement -id 'update-scroll' -type 'ScrollBar' -all_surfaces) -end
    $popup = Get-UpdateSurface
    $popup_box = $popup.Current.BoundingRectangle
    Assert-Layout ($popup_box.Top -ge $work.top -and $popup_box.Bottom -le $work.bottom) 'Multiple response requests escaped the work area.'
    Save-LuilShot -path (Join-Path $work_directory 'multiple-notifications.png') -surface $popup
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -id 'update-count' -all_surfaces -absent
    Send-Update '사용자 입력을 기다립니다' 'input-notice' 'input_requested'
    $null = Wait-LuilElement -name '사용자 입력을 기다립니다'
    Invoke-LuilElement (Wait-LuilElement -id 'widget-collapse')
    $null = Wait-LuilElement -name '사용자 입력을 기다립니다' -all_surfaces
    Assert-Layout (-not (Wait-LuilElement -name '응답 대기' -all_surfaces).Current.IsEnabled) 'Input request was not retained as a persistent notification.'
    # 묻고 끝난 응답도 답을 기다리는 알림이라 접힌 위젯 밖에 띄운다. 답하지 않기로 하면 숨길 수 있다.
    Send-Update '커밋 준비' 'reply-notice'
    Send-Update '바로 커밋할까요?' 'reply-notice' 'turn_finished'
    $null = Wait-LuilElement -name '바로 커밋할까요?' -all_surfaces
    $reply_button = Wait-LuilElement -search 'update-.*-confirm' -name '닫기' -all_surfaces
    Assert-Layout $reply_button.Current.IsEnabled 'A question at the end of an answer cannot be dismissed.'
    Save-LuilShot -path (Join-Path $work_directory 'reply-notification.png') -surface (Get-UpdateSurface)
    Invoke-LuilElement $reply_button
    $null = Wait-LuilElement -name '바로 커밋할까요?' -all_surfaces -absent
    @{
        checks = $checks; desktop = $desktop_name.ToString(); scale = $scale; isolated = $true
        header_top = $true; header_bottom = $true; header_anchor = $true; header_placement = $true; caption_hit = $true; work_area_limit = $true
        notification_above = $true; notification_below = $true; finished_work = $true; notification_scroll = $true
        persistent_response = $true; external_response_expiry = $true; collapse_cycles = 40
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work_directory 'verification.json') -Encoding utf8NoBOM
    Write-Output "$checks widget layout and notification checks passed (isolated desktop: $desktop_name). Artifacts: $work_directory"
}
catch {
    $failure = $_
    Write-Output $failure.ScriptStackTrace
    try { Get-LuilTree -limit 80 } catch { Write-Output $_.Exception.Message }
    throw ($failure.Exception.Message + "`n" + $failure.ScriptStackTrace)
}
