#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppProcessId,
    [Parameter(Mandatory)][string]$ThreadId,
    [Parameter(Mandatory)][string]$ExpectedProgress
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
Add-Type -TypeDefinition @'
using System.Text;
public static class SessionBoardIdentity {
    public static string Key(string id) {
        unchecked {
            ulong hash = 14695981039346656037UL;
            foreach (byte value in Encoding.UTF8.GetBytes(id)) {
                hash ^= value;
                hash *= 1099511628211UL;
            }
            return hash.ToString("x16");
        }
    }
}
'@
$ui_root = Connect-LuilApp -process_id $AppProcessId
$window_handle = [IntPtr]$ui_root.Current.NativeWindowHandle
$owner = 'compact-codex-' + [SessionBoardIdentity]::Key($ThreadId)
$project_root = Split-Path -Parent $PSScriptRoot
try {
    $card = Wait-LuilElement -id $owner -search ([regex]::Escape($ExpectedProgress)) -timeout_seconds 25
    $status = Wait-LuilElement -id ($owner + '-status') -name '작업 중'
    $before = $card.Current.Name
    if ($null -eq (Find-LuilElement -id ($owner + '-identity-0'))) {
        Invoke-LuilElement (Wait-LuilElement -id $owner)
    }
    $identity = Wait-LuilElement -id ($owner + '-identity-0') -search ([regex]::Escape($ThreadId))
    $source = Wait-LuilElement -id ($owner + '-source-detail-0') -search '열린 세션'
    $name = Wait-LuilElement -id ($owner + '-name-0')
    $card = Wait-LuilElement -id $owner
    if ($card.Current.BoundingRectangle.IsEmpty -or $identity.Current.BoundingRectangle.IsEmpty) {
        throw 'The current session or identity is not visibly laid out.'
    }
    Save-LuilShot -path (Join-Path $project_root 'build/current-codex-session.png') | Out-Null
    @{thread_id=$ThreadId;card_id=$owner;conversation=$name.Current.Name;progress=$before;status=$status.Current.Name;source=$source.Current.Name;observed_at=[DateTimeOffset]::UtcNow.ToString('o')} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $project_root 'build/current-session-verification.json') -Encoding utf8NoBOM
    Write-Output ('Current conversation verified: '+$ThreadId)
    Write-Output $before
}
finally {
    Disconnect-LuilApp -keep
    $null = [Luil.Uia.Native]::ShowWindow($window_handle,4)
}
