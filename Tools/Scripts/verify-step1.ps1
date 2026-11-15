#Requires -RunAsAdministrator
<#
.SYNOPSIS
段階 1 の確認: APO を CABLE Input に入れて -6 dB かかることと、外すと元に戻ることを ApoProbe で測る。
あわせて、Audiosrv の再起動だけで APO の付け外しが反映されるかを確かめる。

.DESCRIPTION
音声サービスを何度か再起動するので、そのたびに数秒間 Windows の音が止まる（ASIO は影響を受けない）。
途中で失敗したときは、APO を外して元の状態に戻す。

.EXAMPLE
.\verify-step1.ps1 -Transcript C:\temp\verify-step1.log
#>
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$Transcript
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if ($Transcript) { Start-Transcript -Path $Transcript -Force | Out-Null }
[Console]::OutputEncoding = [Text.Encoding]::UTF8   # ApoProbe は UTF-8 で出力する

$probe = Join-Path $PriRepoRoot "build\bin\$Config\ApoProbe.exe"

# ApoProbe を実行して、送った音との差（dB）を返す。測れなければ $null
function Invoke-Probe([string]$Label) {
    Start-Sleep -Seconds 2   # サービスの起動直後はデバイスの準備に少しかかる
    Write-Host "=== $Label ==="
    $ErrorActionPreference = 'Continue'   # ApoProbe の標準エラー出力で止まらないようにする
    $output = & $probe 2>&1 | ForEach-Object { "$_" }
    $output | ForEach-Object { Write-Host "  $_" }
    $line = $output | Where-Object { $_ -match '^RESULT gain_db=' } | Select-Object -Last 1
    if ($line) {
        return [double]::Parse(($line -replace '^RESULT gain_db=', ''), [Globalization.CultureInfo]::InvariantCulture)
    }
    return $null
}

function Test-ApoLoaded {
    (Test-Path $PriLogFile) -and (Select-String -Path $PriLogFile -Pattern 'LockForProcess: endpoint=' -Quiet)
}

function Show-ApoLog {
    if (Test-Path $PriLogFile) {
        Get-Content $PriLogFile -Encoding UTF8 | Select-Object -Last 20 | ForEach-Object { Write-Host "  log: $_" }
    } else {
        Write-Host '  log: (ログファイルなし)'
    }
}

$results = [ordered]@{}
$installed = $false
try {
    if (Test-Path $PriLogFile) { Remove-Item $PriLogFile -Force }

    $results['1. APO なし（基準）'] = Invoke-Probe '1. APO なし（基準）'

    & (Join-Path $PSScriptRoot 'dev-install.ps1') -Config $Config -Endpoint 'CABLE Input' -TestGainDb -6
    $installed = $true
    $results['2. インストール後 -6 dB（Audiosrv の再起動のみ）'] = Invoke-Probe '2. インストール後 -6 dB（Audiosrv の再起動のみ）'
    $loaded = Test-ApoLoaded
    $results['   Audiosrv の再起動だけで APO が読み込まれたか'] = $loaded

    if (-not $loaded) {
        Write-Host 'Audiosrv の再起動だけでは読み込まれなかったので、AudioEndpointBuilder も再起動します'
        Stop-PriAudio -IncludeEndpointBuilder
        Start-PriAudio
        $results['3. AudioEndpointBuilder も再起動'] = Invoke-Probe '3. AudioEndpointBuilder も再起動'
        $results['   AudioEndpointBuilder の再起動で APO が読み込まれたか'] = Test-ApoLoaded
    }

    $lockCountBefore = @(Select-String -Path $PriLogFile -Pattern 'LockForProcess: endpoint=' -ErrorAction SilentlyContinue).Count
    & (Join-Path $PSScriptRoot 'dev-uninstall.ps1')
    $installed = $false
    $results['4. アンインストール後（Audiosrv の再起動のみ）'] = Invoke-Probe '4. アンインストール後（Audiosrv の再起動のみ）'
    $lockCountAfter = @(Select-String -Path $PriLogFile -Pattern 'LockForProcess: endpoint=' -ErrorAction SilentlyContinue).Count
    $results['   アンインストール後も APO が読み込まれたか'] = ($lockCountAfter -gt $lockCountBefore)
}
catch {
    Write-Host "ERROR: $_"
    Write-Host $_.ScriptStackTrace
    if ($installed) {
        Write-Host '途中で失敗したので APO を外します'
        try { & (Join-Path $PSScriptRoot 'dev-uninstall.ps1') -RestartEndpointBuilder } catch { Write-Host "ERROR (uninstall): $_" }
    }
}
finally {
    Write-Host '=== まとめ ==='
    foreach ($entry in $results.GetEnumerator()) { Write-Host ('{0} : {1}' -f $entry.Key, $entry.Value) }
    Write-Host '=== APO のログ ==='
    Show-ApoLog
    if ($Transcript) { Stop-Transcript | Out-Null }
}
