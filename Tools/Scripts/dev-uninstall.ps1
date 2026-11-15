#Requires -RunAsAdministrator
<#
.SYNOPSIS
開発用: APO を入れたすべてのデバイスを元の状態（Equalizer APO など）に戻し、DLL の登録を解除して削除する。

.DESCRIPTION
音声サービスを再起動するので、数秒間 Windows の音が止まる（ASIO は影響を受けない）。
デバイスを元に戻すのは PRISystemRackSetup.exe detach-all（控えた値をそのまま書き戻す）。
ログ（%ProgramData%\PRISystemRack\Logs）は残す。
#>
param(
    [string]$Config = 'RelWithDebInfo',
    [switch]$RestartEndpointBuilder
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$setupTool = Join-Path $PriRepoRoot "build\PRISystemRack_artefacts\$Config\PRISystemRackSetup.exe"
if (-not (Test-Path $setupTool)) { throw "ビルド出力が見つかりません: $setupTool" }

Write-Host '音声サービスを停止します（数秒間、Windows の音が止まります）'
Stop-PriAudio -IncludeEndpointBuilder:$RestartEndpointBuilder
try {
    & $setupTool detach-all --no-restart | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "PRISystemRackSetup detach-all が失敗しました（$LASTEXITCODE）" }

    $dll = Join-Path $PriInstallDir $PriDllName
    if (Test-Path $dll) {
        $p = Start-Process regsvr32.exe -ArgumentList '/u', '/s', "`"$dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Warning "regsvr32 /u が失敗しました（$($p.ExitCode)）" }
    }
    if (Test-Path $PriInstallDir) { Remove-Item $PriInstallDir -Recurse -Force }
    if (Test-Path $PriRegRoot) { Remove-Item $PriRegRoot -Recurse }
}
finally {
    Start-PriAudio
}
Write-Host "アンインストールしました（ログ $PriLogDir は残しています）"
