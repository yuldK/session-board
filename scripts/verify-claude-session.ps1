#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$AppProcessId,
    [Parameter(Mandatory)][string]$SessionId,
    # 카드 본문에 나와야 할 공개 진행 메시지의 정규식이다. 비우면 내용은 확인하지 않는다.
    [string]$ExpectedProgress,
    [string]$OutputName = 'current-claude-session',
    [string]$UiaScript = (Join-Path $env:USERPROFILE '.codex/skills/luil-uia/scripts/uia.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. $UiaScript
Add-Type -TypeDefinition @'
using System.Text;
public static class SessionBoardClaudeIdentity {
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
$owner = 'compact-claude-' + [SessionBoardClaudeIdentity]::Key($SessionId)
$project_root = Split-Path -Parent $PSScriptRoot
try {
    $card = if ($ExpectedProgress) { Wait-LuilElement -id $owner -search $ExpectedProgress -timeout_seconds 25 } else { Wait-LuilElement -id $owner -timeout_seconds 25 }
    $card_name = $card.Current.Name
    $status = (Wait-LuilElement -id ($owner + '-status')).Current.Name
    $activity = (Wait-LuilElement -id ($owner + '-activity')).Current.Name
    if ($null -eq (Find-LuilElement -id ($owner + '-identity-0'))) {
        Invoke-LuilElement (Wait-LuilElement -id $owner)
    }
    $identity = Wait-LuilElement -id ($owner + '-identity-0') -search ([regex]::Escape($SessionId))
    $source = Wait-LuilElement -id ($owner + '-source-detail-0') -search 'Claude 로컬 기록'
    $folder = Wait-LuilElement -id ($owner + '-folder-0')
    $card = Wait-LuilElement -id $owner
    if ($card.Current.BoundingRectangle.IsEmpty -or $identity.Current.BoundingRectangle.IsEmpty) {
        throw 'The Claude session card is not visibly laid out.'
    }
    $buttons = $ui_root.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Button))
    $codex_cards = @($buttons | Where-Object { $_.Current.AutomationId -match ':compact-codex-[0-9a-f]{16}$' })
    $signal = Find-LuilElement -id 'widget-signal-claude'
    Save-LuilShot -path (Join-Path $project_root "build/$OutputName.png") | Out-Null
    # 전체 대화·요청 원문·자격 증명은 남기지 않는다. 카드에 보인 짧은 글만 남긴다.
    $result = [ordered]@{
        observed_at = [DateTimeOffset]::UtcNow.ToString('o')
        session_id  = $SessionId
        card_id     = $owner
        card        = $card_name
        status      = $status
        activity    = $activity
        source      = $source.Current.Name
        folder      = $folder.Current.Name
        identity    = $identity.Current.Name
        codex_cards = $codex_cards.Count
        signal      = if ($null -ne $signal) { $signal.Current.Name } else { $null }
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $project_root "build/$OutputName-verification.json") -Encoding utf8NoBOM
    $result | ConvertTo-Json -Depth 4
}
finally {
    Disconnect-LuilApp -keep
    # UIA 정리가 창을 최소화했으면 활성화하지 않고 다시 보인다.
    $null = [Luil.Uia.Native]::ShowWindow($window_handle, 4)
}
