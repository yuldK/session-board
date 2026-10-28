#requires -Version 7.2
# 아래쪽 머릿줄의 접기·펼치기 중 DWM이 합성한 화면에서 머릿줄이 다른 자리에 보이는지 잰다.
#  - 격리 데스크톱과 창 DC로는 합성된 화면을 볼 수 없어 사용 중인 데스크톱에 테스트 위젯을 잠시 띄운다.
#  - 별도 데이터 폴더로 띄우므로 실제 위젯 자리·외형 설정과 Hook 설정은 건드리지 않는다. 끝나면 테스트 앱을 닫는다.
[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '../build/vs2026/bin/Release/session-board.exe'),
    [int]$Cycles = 20,
    [int]$WatchMs = 200
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace SessionBoardFlashTest {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Monitor { public uint size; public Rect monitor, work; public uint flags; }
    public static class Native {
        [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
        [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr monitor, ref Monitor value);
    }
}
'@
$null = [SessionBoardFlashTest.Native]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))
$app_path = (Resolve-Path -LiteralPath $Executable).Path
$work_directory = Join-Path (Split-Path -Parent $PSScriptRoot) ('build/widget-flash-' + [guid]::NewGuid().ToString('N'))
$data_directory = Join-Path $work_directory 'sessions'
$null = New-Item -ItemType Directory -Path $data_directory -Force
@{
    schema_version = 1; provider = 'codex'; session_id = 'flash-test'; event = 'turn_started'
    timestamp_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); summary = '첫 번째 작업'; cwd = $work_directory
} | ConvertTo-Json -Compress | & $app_path --hook --normalized --strict --data-dir $data_directory | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Test hook failed.' }

function Get-Shot([int]$x, [int]$y, [int]$width, [int]$height) {
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($x, $y, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    return $bitmap
}

# 표본 열에서 머릿줄 색과 같은 줄을 찾는다.
function Get-HeaderRows([System.Drawing.Bitmap]$bitmap, [int]$column, [System.Drawing.Color]$accent) {
    $rows = [System.Collections.Generic.List[int]]::new()
    for ($row = 0; $row -lt $bitmap.Height; $row += 4) {
        $pixel = $bitmap.GetPixel($column, $row)
        if ([Math]::Abs($pixel.R - $accent.R) + [Math]::Abs($pixel.G - $accent.G) + [Math]::Abs($pixel.B - $accent.B) -lt 12) { $rows.Add($row) }
    }
    return , $rows
}

$process = Start-Process -FilePath $app_path -ArgumentList ('--data-dir "{0}" --position=80,80' -f $data_directory) -PassThru
try {
    $automation = [System.Windows.Automation.AutomationElement]
    $root = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ($null -eq $root -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $condition = [System.Windows.Automation.PropertyCondition]::new($automation::ProcessIdProperty, $process.Id)
        $root = $automation::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
    }
    if ($null -eq $root) { throw 'The test widget did not appear.' }
    $window = [IntPtr]$root.Current.NativeWindowHandle
    $monitor = [SessionBoardFlashTest.Monitor]::new()
    $monitor.size = [Runtime.InteropServices.Marshal]::SizeOf($monitor)
    $null = [SessionBoardFlashTest.Native]::GetMonitorInfoW([SessionBoardFlashTest.Native]::MonitorFromWindow($window, 2), [ref]$monitor)
    $scale = [SessionBoardFlashTest.Native]::GetDpiForWindow($window) / 96.0
    $caption = [int](40 * $scale)
    $width = [int](400 * $scale)
    $height = [int](560 * $scale)
    $x = $monitor.work.left + 50
    $header_y = $monitor.work.bottom - $caption - 50
    # 아래가 모자란 자리로 옮겨 목록을 머릿줄 위로 펼치게 한다.
    $null = [SessionBoardFlashTest.Native]::SetWindowPos($window, [IntPtr]::Zero, $x, $header_y, $width, $height, 0x14)
    Start-Sleep -Milliseconds 800
    $region_top = $header_y + $caption - $height - 10
    $region_height = $height + 20
    $column = [int](8 * $scale)
    $header_row = $header_y + [int]($caption / 2) - $region_top
    $settled = Get-Shot $x $region_top $width $region_height
    $accent = $settled.GetPixel($column, $header_row)
    $settled.Save((Join-Path $work_directory 'header-bottom.png'))
    $settled.Dispose()
    $button = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition) |
        Where-Object { $_.Current.AutomationId -match '^\d+:widget-collapse$' } | Select-Object -First 1
    if ($null -eq $button) { throw 'The collapse button was not found.' }

    $misplaced = 0
    $hidden = 0
    $shots = 0
    foreach ($toggle in 1..($Cycles * 2)) {
        $kind = $toggle % 2 -eq 0 ? 'expand' : 'collapse'
        $captured = [System.Collections.Generic.List[object]]::new()
        $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($watch.ElapsedMilliseconds -lt $WatchMs) {
            $captured.Add(@($watch.ElapsedMilliseconds, (Get-Shot $x $region_top $width $region_height)))
        }
        foreach ($shot in $captured) {
            $shots++
            $rows = Get-HeaderRows $shot[1] $column $accent
            $away = @($rows | Where-Object { [Math]::Abs($_ - $header_row) -gt $caption })
            if ($away.Count -gt 0) {
                $misplaced++
                Write-Output ("{0} {1} t={2}ms: header color at rows {3}..{4}, expected around {5}" -f $kind, $toggle, $shot[0], $away[0], $away[-1], $header_row)
                $shot[1].Save((Join-Path $work_directory "misplaced-$toggle-$($shot[0]).png"))
            }
            elseif (-not ($rows | Where-Object { [Math]::Abs($_ - $header_row) -le 8 })) {
                $hidden++
            }
            $shot[1].Dispose()
        }
    }
    @{ toggles = $Cycles * 2; shots = $shots; misplaced = $misplaced; hidden = $hidden; scale = $scale } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work_directory 'verification.json') -Encoding utf8NoBOM
    if ($misplaced -gt 0) { throw "The header appeared away from its place in $misplaced captured frames. Artifacts: $work_directory" }
    Write-Output "No misplaced header in $shots frames over $($Cycles * 2) toggles ($hidden frames without the header while hidden). Artifacts: $work_directory"
}
finally {
    if (-not $process.HasExited) { $process.Kill() }
}
