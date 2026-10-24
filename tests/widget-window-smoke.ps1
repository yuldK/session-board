#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppProcessId,
    [string]$UiaScript = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. $UiaScript
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace SessionBoardDragTest {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    public static class Native {
        [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect value);
        [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point value);
        [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
        [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
        [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point value);
        [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr word, IntPtr data);
        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    }
}
'@
$null = [SessionBoardDragTest.Native]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))
$ui_root = Connect-LuilApp -process_id $AppProcessId
$window_handle = [IntPtr]$ui_root.Current.NativeWindowHandle
$original = [SessionBoardDragTest.Rect]::new()
$cursor = [SessionBoardDragTest.Point]::new()
$null = [SessionBoardDragTest.Native]::GetWindowRect($window_handle, [ref]$original)
$null = [SessionBoardDragTest.Native]::GetCursorPos([ref]$cursor)
$project_root = Split-Path -Parent $PSScriptRoot
try {
    $title = (Wait-LuilElement -id 'widget-title').Current.BoundingRectangle
    # UIA가 다루지 않는 네이티브 캡션 끌기만 실제 포인터로 검사한다.
    $point = [SessionBoardDragTest.Point]::new()
    $point.x = [int]($title.Right - 8)
    $point.y = [int]($title.Y + $title.Height / 2)
    $packed = [IntPtr]::new(($point.y -shl 16) -bor ($point.x -band 65535))
    $hit = [SessionBoardDragTest.Native]::SendMessageW($window_handle, 0x84, [IntPtr]::Zero, $packed).ToInt64()
    if ($hit -ne 2 -or [SessionBoardDragTest.Native]::WindowFromPoint($point) -ne $window_handle) {
        throw 'The blank caption area is transparent to the pointer or does not expose HTCAPTION.'
    }
    $null = [SessionBoardDragTest.Native]::SetCursorPos($point.x, $point.y)
    [SessionBoardDragTest.Native]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    $null = [SessionBoardDragTest.Native]::SetCursorPos($point.x + 48, $point.y + 32)
    Start-Sleep -Milliseconds 80
    [SessionBoardDragTest.Native]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $moved = [SessionBoardDragTest.Rect]::new()
        $null = [SessionBoardDragTest.Native]::GetWindowRect($window_handle, [ref]$moved)
        if ($moved.left -ne $original.left -or $moved.top -ne $original.top) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($moved.left - $original.left -ne 48 -or $moved.top - $original.top -ne 32) {
        throw "Caption drag did not move the window by the expected distance: $($moved.left - $original.left), $($moved.top - $original.top)"
    }
    @{ caption_hit = $hit; blank_caption_receives_pointer = $true; moved_x = ($moved.left - $original.left); moved_y = ($moved.top - $original.top); dpi = [SessionBoardDragTest.Native]::GetDpiForWindow($window_handle) } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $project_root 'build/widget-window-verification.json') -Encoding utf8NoBOM
    Write-Output 'Native caption drag passed: blank caption receives the pointer and the window moved by 48,32 physical pixels.'
}
finally {
    [SessionBoardDragTest.Native]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    $null = [SessionBoardDragTest.Native]::SetWindowPos($window_handle, [IntPtr]::Zero, $original.left, $original.top, 0, 0, 0x15)
    $null = [SessionBoardDragTest.Native]::SetCursorPos($cursor.x, $cursor.y)
    Disconnect-LuilApp
    $null = [Luil.Uia.Native]::ShowWindow($window_handle, 4)
}
