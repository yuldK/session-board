#requires -Version 7.2

# luil-uia의 격리 실행에 앱 인자를 더한다. 실제 저장소·Hook 설정과 사용 중인 데스크톱은 건드리지 않는다.
function Connect-SessionBoardIsolated {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string]$Arguments,
        [Parameter(Mandatory)][string]$UiaScript
    )

    $script:luil_desktop_name = 'luil-uia-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $script:luil_desktop = [Luil.Uia.Native]::CreateDesktopW($script:luil_desktop_name, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($script:luil_desktop -eq [IntPtr]::Zero) {
        throw 'The isolated desktop could not be created.'
    }
    $script:luil_isolated = $true
    $queue_root = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'luil-uia'))
    $script:luil_queue = [IO.Path]::GetFullPath((Join-Path $queue_root $script:luil_desktop_name))
    if (-not $script:luil_queue.StartsWith($queue_root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The isolated UIA queue escaped its temporary directory.'
    }
    $null = New-Item -ItemType Directory -Path $script:luil_queue -Force
    $script:luil_step_id = 0
    $script:luil_root = $null
    $script:luil_process = Start-LuilProcess -path $Executable -arguments $Arguments -desktop $script:luil_desktop_name
    $script:luil_started_app = $true
    $worker_path = (Get-Process -Id $PID).Path
    $worker_script = Join-Path (Split-Path -Parent $UiaScript) 'desktop_worker.ps1'
    $worker_arguments = '-NoProfile -STA -File "{0}" -queue "{1}" -process_id {2} -timeout_seconds 20' -f $worker_script, $script:luil_queue, $script:luil_process.Id
    $script:luil_worker = Start-LuilProcess -path $worker_path -arguments $worker_arguments -desktop $script:luil_desktop_name -no_window
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path (Join-Path $script:luil_queue 'ready.txt')) { return }
        if (Test-Path (Join-Path $script:luil_queue 'failed.txt')) {
            throw ('The isolated UIA worker failed: ' + (Get-Content (Join-Path $script:luil_queue 'failed.txt') -Raw))
        }
        if ($script:luil_process.HasExited -or $script:luil_worker.HasExited) {
            throw 'The isolated app or UIA worker exited before becoming ready.'
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'The isolated UIA worker was not ready in time.'
}
