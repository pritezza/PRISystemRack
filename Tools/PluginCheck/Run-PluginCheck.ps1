# VST3 load check: tries every VST3 plugin on this PC the same way PRI System Rack loads it
# (PRISystemRackScanner.exe --check-all, see Source/Scanner/PluginCheck.h).
#
# - Runs in its own console window. Close the window to stop; run this script again to continue where it stopped.
# - Results: <project>\VST3PluginCheck.md (rewritten after each plugin).
# - Progress data: %LOCALAPPDATA%\PRISystemRack\PluginCheck\results.tsv (delete the folder to start over).
# - The scanner exe is copied into that folder and run from there, so rebuilding the app does not fail on a locked exe.
# - -RetryTimeouts: also try again the plugins (and files) that timed out last time.
# - To skip files, list their file names (one per line) in skip.txt in that folder (takes effect while running).
param([switch]$RetryTimeouts)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$built = Join-Path $root 'build\PRISystemRack_artefacts\Release\PRISystemRackScanner.exe'
$work = Join-Path $env:LOCALAPPDATA 'PRISystemRack\PluginCheck'
$report = Join-Path $root 'VST3PluginCheck.md'
$exe = Join-Path $work 'PRISystemRackScanner.exe'

if (-not (Test-Path $built)) { Write-Host "Not built yet: $built"; exit 1 }
New-Item -ItemType Directory -Force $work | Out-Null

$running = Get-Process -Name PRISystemRackScanner -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if ($running) { Write-Host 'The check is already running.'; exit 1 }

Copy-Item $built $exe -Force
$env:PRI_CHECK_RETRY_TIMEOUTS = if ($RetryTimeouts) { '1' } else { '' }
Start-Process -FilePath $exe -ArgumentList @('--check-all', "`"$work`"", "`"$report`"")
Write-Host "Started. Results: $report"
