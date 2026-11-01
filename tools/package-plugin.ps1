# SPDX-License-Identifier: Apache-2.0
# Packages the built plugin: dist\obs-tab-player-<version>-windows-x64.zip (Install.cmd inside)
# and, when Inno Setup's ISCC.exe is available, dist\obs-tab-player-<version>-windows-x64-setup.exe.
#   powershell -File tools\package-plugin.ps1 [-Version 0.2.0] [-Build build\plugin\Release]
param([string]$Version = '0.2.0', [string]$Build = 'build\plugin\Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$dll = Join-Path $Build 'obs-tab-player.dll'
if (-not (Test-Path $dll)) { throw "Build the plugin first: $dll not found" }

$stage = Join-Path $root 'dist\stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage 'obs-tab-player\bin\64bit') | Out-Null
Copy-Item $dll (Join-Path $stage 'obs-tab-player\bin\64bit\')
Copy-Item -Recurse 'plugin\data' (Join-Path $stage 'obs-tab-player\data')
Copy-Item 'plugin\packaging\Install.cmd', 'plugin\packaging\Uninstall.cmd', 'plugin\packaging\README.txt' $stage
Copy-Item 'LICENSE' (Join-Path $stage 'LICENSE.txt')
New-Item -ItemType Directory -Force (Join-Path $stage 'demo-tabs') | Out-Null
Copy-Item 'docs\demo\*.gp5' (Join-Path $stage 'demo-tabs')

$zip = Join-Path $root "dist\obs-tab-player-$Version-windows-x64.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
"zip:   $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"

$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $iscc) { $iscc = Get-Item "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" -ErrorAction SilentlyContinue }
if ($iscc) {
  & $iscc.Source /Q "/DAppVersion=$Version" "/DStageDir=$stage" 'plugin\packaging\installer.iss'
  "setup: dist\obs-tab-player-$Version-windows-x64-setup.exe"
} else {
  'setup: skipped (Inno Setup not installed)'
}
