# 開発用スクリプトの共通定義（各スクリプトから dot-source して使う）
# 注意: Windows PowerShell 5.1 で日本語を正しく読ませるため、このフォルダの .ps1 は UTF-8（BOM 付き）で保存する

$PriClsid      = '{C06E99BA-ABC5-4FB7-9943-0306C073ADA5}'
$PriDllName    = 'PRISystemRackAPO.dll'
$PriInstallDir = Join-Path $env:ProgramFiles 'PRISystemRack'
$PriLogDir     = Join-Path $env:ProgramData 'PRISystemRack\Logs'
$PriLogFile    = Join-Path $PriLogDir 'apo.log'
$PriRegRoot    = 'HKLM:\SOFTWARE\PRISystemRack'
$PriRepoRoot   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$RenderRoot    = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$DeviceDescKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'   # PKEY_Device_DeviceDesc（例: "CABLE Input"）

# 有効な再生デバイスを名前（"CABLE Input" など）で探す。1 つに定まらなければ例外
function Get-PriRenderEndpoint([string]$Name) {
    $found = @(Get-ChildItem $RenderRoot | Where-Object {
        (Get-ItemProperty $_.PSPath).DeviceState -eq 1 -and
        (Get-ItemProperty "$($_.PSPath)\Properties" -ErrorAction SilentlyContinue).$DeviceDescKey -eq $Name
    })
    if ($found.Count -ne 1) {
        throw "有効な再生デバイス '$Name' が 1 つに定まりません（見つかった数: $($found.Count)）"
    }
    $found[0]
}

# 音声サービスを止め、audiodg.exe（APO を読み込んでいるプロセス）の終了を待つ
function Stop-PriAudio([switch]$IncludeEndpointBuilder) {
    if ($IncludeEndpointBuilder) {
        Stop-Service AudioEndpointBuilder -Force   # 依存している Audiosrv も止まる
    } else {
        Stop-Service Audiosrv -Force
    }
    $audiodg = Get-Process audiodg -ErrorAction SilentlyContinue
    if ($audiodg) { $audiodg | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue }
    if (Get-Process audiodg -ErrorAction SilentlyContinue) { throw 'audiodg.exe が終了しません' }
}

function Start-PriAudio {
    Start-Service AudioEndpointBuilder
    Start-Service Audiosrv
}
