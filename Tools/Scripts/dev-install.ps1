#Requires -RunAsAdministrator
<#
.SYNOPSIS
開発用: ビルドした APO を Program Files にコピーして登録し、指定した再生デバイスのミックス後の枠に入れる。

.DESCRIPTION
- 音声サービスを再起動するので、数秒間 Windows の音が止まる（ASIO は影響を受けない）。
- デバイスへの付け外しは PRISystemRackSetup.exe が行う。今の登録（Equalizer APO など）は控えてから外し、
  dev-uninstall.ps1 や本体アプリの「有効」のチェックを外すと元に戻せる。
- -Endpoint '' なら DLL の登録だけ行う（デバイスへの付け外しは本体アプリから行う）。
- 何度実行してもよい（DLL の更新にも使う）。

.EXAMPLE
.\dev-install.ps1 -Endpoint 'CABLE Input' -TestGainDb -6
.\dev-install.ps1 -Endpoint ''
#>
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$Endpoint = 'CABLE Input',
    [double]$TestGainDb = 0,
    [switch]$RestartEndpointBuilder
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$builtDll = Join-Path $PriRepoRoot "build\bin\$Config\$PriDllName"
if (-not (Test-Path $builtDll)) { throw "ビルド出力が見つかりません: $builtDll" }
$setupTool = Join-Path $PriRepoRoot "build\PRISystemRack_artefacts\$Config\PRISystemRackSetup.exe"
if ($Endpoint -ne '' -and -not (Test-Path $setupTool)) { throw "ビルド出力が見つかりません: $setupTool" }

$ep = $null
if ($Endpoint -ne '') { $ep = Get-PriRenderEndpoint $Endpoint }

Write-Host '音声サービスを停止します（数秒間、Windows の音が止まります）'
Stop-PriAudio -IncludeEndpointBuilder:$RestartEndpointBuilder
try {
    # DLL（と PDB）をコピーして、COM クラスと APO を登録する
    New-Item -ItemType Directory -Force $PriInstallDir | Out-Null
    Copy-Item $builtDll $PriInstallDir -Force
    $pdb = [IO.Path]::ChangeExtension($builtDll, '.pdb')
    if (Test-Path $pdb) { Copy-Item $pdb $PriInstallDir -Force }

    # audiodg.exe は LOCAL SERVICE（S-1-5-19）で動くので、ログフォルダへの書き込みを許可する
    New-Item -ItemType Directory -Force $PriLogDir | Out-Null
    & icacls.exe $PriLogDir /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls が失敗しました（$LASTEXITCODE）" }

    $dll = Join-Path $PriInstallDir $PriDllName
    $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 が失敗しました（$($p.ExitCode)）" }

    # 確認用のゲイン（APO が LockForProcess のときに読む）
    $apoKey = "$PriRegRoot\APO"
    if (-not (Test-Path $apoKey)) { New-Item $apoKey -Force | Out-Null }
    $gainText = $TestGainDb.ToString([Globalization.CultureInfo]::InvariantCulture)
    New-ItemProperty $apoKey -Name TestGainDb -Value $gainText -PropertyType String -Force | Out-Null

    # デバイスに入れる（今の登録は控えてから外す）
    if ($ep) {
        & $setupTool attach $ep.PSChildName --no-restart | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "PRISystemRackSetup attach が失敗しました（$LASTEXITCODE）" }
    }
}
finally {
    Start-PriAudio
}

if ($ep) {
    Write-Host "インストールしました: $Endpoint $($ep.PSChildName) / TestGainDb=$gainText dB"
} else {
    Write-Host "DLL を登録しました（デバイスには入れていません）/ TestGainDb=$gainText dB"
}
