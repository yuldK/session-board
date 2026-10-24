[CmdletBinding()]
param(
    [switch]$check,
    [string]$clang_format
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot
if (-not $clang_format) {
    $candidate = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($candidate) {
        $clang_format = $candidate.Source
    }
    else {
        $candidates = @(Get-ChildItem -Path "$env:ProgramFiles/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-format.exe" -ErrorAction SilentlyContinue)
        if ($candidates.Count -gt 0) {
            foreach ($tool in $candidates) {
                $tool_version = & $tool.FullName --version
                if ($tool_version -match 'version 22\.') {
                    $clang_format = $tool.FullName
                    break
                }
            }
        }
    }
}
if (-not $clang_format -or -not (Test-Path -LiteralPath $clang_format)) {
    throw 'clang-format 22 is required. Install the Visual Studio C++ Clang tools or pass -clang_format.'
}
$version_text = & $clang_format --version
if ($version_text -notmatch 'version 22\.') {
    throw "clang-format 22 is required: $version_text"
}
$sources = @(Get-ChildItem -LiteralPath (Join-Path $project_root 'src'), (Join-Path $project_root 'tests') -Recurse -File | Where-Object { $_.Extension -in '.cpp', '.h' })
foreach ($source in $sources) {
    if ($check) {
        & $clang_format --dry-run --Werror $source.FullName
    }
    else {
        & $clang_format -i $source.FullName
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Formatting failed: $($source.FullName)"
    }
}
Write-Output "Formatted $($sources.Count) source files (check=$check)."
