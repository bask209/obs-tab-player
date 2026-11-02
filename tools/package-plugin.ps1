# SPDX-License-Identifier: Apache-2.0
# Packages the built plugin into dist\:
#   obs-tab-player-<version>-windows-x64-setup.exe  (Inno Setup 6: the main download)
#   obs-tab-player-<version>-windows-x64.zip        (Install.cmd inside, for people who prefer no installer)
#   SHA256SUMS.txt
#   powershell -File tools\package-plugin.ps1 [-Build build\plugin\Release] [-NoSetup]
param([string]$Build = 'build\plugin\Release', [switch]$NoSetup)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$Version = (Get-Content -Raw package.json | ConvertFrom-Json).version
$dll = Join-Path $Build 'obs-tab-player.dll'
if (-not (Test-Path $dll)) { throw "Build the plugin first: $dll not found" }

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
Get-ChildItem $dist -Filter "obs-tab-player-*" -ErrorAction SilentlyContinue | Remove-Item -Force
New-Item -ItemType Directory -Force (Join-Path $stage 'obs-tab-player\bin\64bit') | Out-Null
Copy-Item $dll (Join-Path $stage 'obs-tab-player\bin\64bit\')
$pdb = Join-Path $Build 'obs-tab-player.pdb'
if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $stage 'obs-tab-player\bin\64bit\') }
Copy-Item -Recurse 'plugin\data' (Join-Path $stage 'obs-tab-player\data')
Copy-Item 'plugin\packaging\Install.cmd', 'plugin\packaging\Uninstall.cmd', 'plugin\packaging\README.txt' $stage
Copy-Item 'LICENSE' (Join-Path $stage 'LICENSE.txt')
New-Item -ItemType Directory -Force (Join-Path $stage 'demo-tabs') | Out-Null
Copy-Item 'docs\demo\*.gp5' (Join-Path $stage 'demo-tabs')

$zip = Join-Path $dist "obs-tab-player-$Version-windows-x64.zip"
# bsdtar (built into Windows 10+) writes portable ZIP entries ('/' separators).
& (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $zip -C $stage obs-tab-player demo-tabs Install.cmd Uninstall.cmd README.txt LICENSE.txt
if ($LASTEXITCODE) { throw 'zip failed' }
"zip:   $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"

if (-not $NoSetup) {
  # Inno Setup 6: preinstalled on GitHub's Windows runners; locally `winget install JRSoftware.InnoSetup`.
  $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: winget install JRSoftware.InnoSetup, or pass -NoSetup' }
  & $iscc /Q "/DAppVersion=$Version" "/DStageDir=$stage" 'plugin\packaging\installer.iss'
  if ($LASTEXITCODE) { throw 'installer build failed' }
  "setup: dist\obs-tab-player-$Version-windows-x64-setup.exe ($([math]::Round((Get-Item "dist\obs-tab-player-$Version-windows-x64-setup.exe").Length / 1KB)) KB)"
}

Get-ChildItem $dist -File | Where-Object { $_.Name -like 'obs-tab-player-*' } | ForEach-Object {
  '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii (Join-Path $dist 'SHA256SUMS.txt')
Get-Content (Join-Path $dist 'SHA256SUMS.txt')
