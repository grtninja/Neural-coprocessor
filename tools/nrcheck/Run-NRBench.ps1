# Run-NRBench.ps1 - MGPU Bridge DLSS-NR bench (experimental, R268)
#
# Put this script, nrcheck.exe and nvngx.dll_nrcheck.dll in the game folder,
# beside the mgpu\ folder (the same folder as the add-on). Close the game.
# Right-click this file > Run with PowerShell.
#
# It measures what one GPU sustains for DLSS-NR at a resolution, with no game
# running. It reads nothing from the game and changes nothing. It writes
# nrbench_result.ini and nrbench_log.txt beside nrcheck.exe.
#
# Parameters (all optional):
#   -Neural <index>   the adapter index of the GPU that does the neural work
#                     (the index nrcheck_report.txt shows). Asked if not given.
#   -Other  <index>   the GPU the game renders on. Its device is created first,
#                     as a game would. Asked if not given; -1 = none.
#   -Res    WxH       default 2560x1440
#   -Passes 1|2       default 1
#   -Frames N         default 300

param(
    [int]$Neural = -2,
    [int]$Other = -2,
    [string]$Res = '2560x1440',
    [int]$Passes = 1,
    [int]$Frames = 300
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$exe = Join-Path $here 'nrcheck.exe'
$dll = Join-Path $here 'nvngx.dll_nrcheck.dll'
foreach ($f in @($exe, $dll)) {
    if (-not (Test-Path $f)) {
        Write-Host "Missing: $f" -ForegroundColor Red
        Write-Host "Keep nrcheck.exe, nvngx.dll_nrcheck.dll and this script together."
        Read-Host 'Press Enter to close'
        exit 1
    }
}

$snippet = $null
foreach ($p in @((Join-Path $here 'mgpu\nvngx_dlssnr.dll'), (Join-Path $here 'nvngx_dlssnr.dll'))) {
    if (Test-Path $p) { $snippet = $p; break }
}
if (-not $snippet) {
    Write-Host 'nvngx_dlssnr.dll not found in mgpu\ or in this folder.' -ForegroundColor Red
    Write-Host 'Run this from the game folder where the add-on is installed.'
    Read-Host 'Press Enter to close'
    exit 1
}

# The GPUs, by the index the check uses (DXGI enumeration order).
Write-Host 'GPUs on this PC (index = the number to give below):'
$i = 0
foreach ($g in (Get-CimInstance Win32_VideoController)) {
    Write-Host ("  " + $g.Name)
    $i++
}
Write-Host 'The indices are the ones nrcheck_report.txt shows as adapter[n]. If you have not run the check yet, run Verify-NRCheck.ps1 first.'
Write-Host ''

if ($Neural -lt -1) { $Neural = [int](Read-Host 'Index of the GPU that does the neural work') }
if ($Other -lt -1)  { $Other  = [int](Read-Host 'Index of the GPU the game renders on (-1 for none)') }

$log = Join-Path $here 'nrbench_log.txt'
$out = Join-Path $here 'nrbench_result.ini'
if (Test-Path $log) { Remove-Item $log }

Write-Host ''
Write-Host "Bench: neural adapter $Neural, other $Other, $Res, $Passes pass(es), $Frames frames" -ForegroundColor Cyan
& $exe --bench --neural $Neural --other $Other --res $Res --passes $Passes --frames $Frames --snippet $snippet --out $out
$code = $LASTEXITCODE

Write-Host ''
if (Test-Path $out) {
    Write-Host "Result written to: $out" -ForegroundColor Green
    Get-Content $out | Where-Object { $_ -match '^(AdapterName|Width|Height|Passes|FramesMeasured|Pass\dMeanMs|FrameMeanMs|BudgetFps|Result)=' } | ForEach-Object { Write-Host "  $_" }
} else {
    Write-Host "No result file. Exit code $code. See nrbench_log.txt." -ForegroundColor Yellow
}
Read-Host 'Press Enter to close'
