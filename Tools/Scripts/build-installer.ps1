<#
.SYNOPSIS
配布用のインストーラー（dist\PRISystemRack-Setup-<版>.exe）を作る。管理者の権限は要らない。

.DESCRIPTION
- 開発用の build フォルダーとは別の build-dist フォルダーに、LTO を有効にして（-DPRI_ENABLE_LTO=ON）Release でビルドする。
  開発用のビルドは LTO を切ったまま速く回せるように、フォルダーを分ける。
- 版は CMakeLists.txt の project(... VERSION x.y.z) から読む。
- Inno Setup 6 の ISCC.exe（winget install JRSoftware.InnoSetup）でコンパイルする。
- -SkipBuild で、ビルドを省いて build-dist の今の成果物からインストーラーだけ作り直す。

.EXAMPLE
.\build-installer.ps1
.\build-installer.ps1 -SkipBuild
#>
param(
    [switch]$SkipBuild,
    [string]$BuildDir = 'build-dist'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$build = Join-Path $repo $BuildDir

# 版
$cmakeText = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw -Encoding UTF8
if ($cmakeText -notmatch 'project\(PRISystemRack VERSION (\d+\.\d+\.\d+)') { throw 'CMakeLists.txt から版を読めません' }
$version = $Matches[1]

# ISCC.exe（ユーザー単位・PC 全体のどちらに入っていてもよい）
$iscc = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 が見つかりません（winget install JRSoftware.InnoSetup）' }

if (-not $SkipBuild) {
    Write-Host "ビルドします（$build、LTO 有効、Release）"
    cmake -S $repo -B $build -G 'Visual Studio 18 2026' -A x64 -DPRI_ENABLE_LTO=ON
    if ($LASTEXITCODE -ne 0) { throw "cmake の構成に失敗しました（$LASTEXITCODE）" }
    # JUCE が作る exe のバージョン情報のリソースは、版を変えても作り直されない（2026-11-13、1.0.0 にしても 0.1.0 のままだった）。
    # 消しておくとビルドのときに今の版で作り直される
    Remove-Item (Join-Path $build 'PRISystemRack_artefacts\JuceLibraryCode\*_resources.rc') -ErrorAction SilentlyContinue
    cmake --build $build --config Release --target PRISystemRack PRISystemRackScanner PRISystemRackSetup PRISystemRackAPO
    if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました（$LASTEXITCODE）" }
}

$appDir = Join-Path $build 'PRISystemRack_artefacts\Release'
$apoDir = Join-Path $build 'bin\Release'
foreach ($file in @("$appDir\PRISystemRack.exe", "$appDir\PRISystemRackScanner.exe", "$appDir\PRISystemRackSetup.exe", "$apoDir\PRISystemRackAPO.dll")) {
    if (-not (Test-Path $file)) { throw "ビルドの成果物が見つかりません: $file" }
}

$outputDir = Join-Path $repo 'dist'
$defines = @(
    "/DAppVersion=$version",
    "/DAppDir=$appDir",
    "/DApoDir=$apoDir",
    "/DOutputDir=$outputDir"
)
$icon = Join-Path $build 'PRISystemRack_artefacts\JuceLibraryCode\icon.ico'
if (Test-Path $icon) { $defines += "/DIconFile=$icon" }

Write-Host "インストーラーを作ります（版 $version）"
& $iscc /Q @defines (Join-Path $repo 'Installer\PRISystemRack.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC が失敗しました（$LASTEXITCODE）" }

$setup = Join-Path $outputDir "PRISystemRack-Setup-$version.exe"
Get-Item $setup | Select-Object FullName, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
