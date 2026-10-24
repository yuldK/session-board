[CmdletBinding()]
param(
    [string]$root = (Join-Path $PSScriptRoot '..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
& (Join-Path $root 'third_party/luil/scripts/check_source_style.ps1') -root $root
exit $LASTEXITCODE
