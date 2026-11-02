# SPDX-License-Identifier: Apache-2.0
# Downloads and builds the OBS Studio SDK the plugin compiles against: libobs and
# obs-frontend-api from the OBS sources, plus OBS's prebuilt dependencies and Qt 6.
# Every download is checked against a pinned SHA-256.
#   powershell -File tools\obs-sdk.ps1 [-Dest .deps] [-Cache <folder with the zips>]
# Afterwards: cmake -S plugin -B build/plugin -DCMAKE_PREFIX_PATH="<Dest>;<Dest>\qt6;<Dest>\deps"
param([string]$Dest = '.deps', [string]$Cache = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$obsVersion = '31.1.1'
$depsVersion = '2025-07-11'
$files = @(
  @{ Name = "$obsVersion.zip"; Url = "https://github.com/obsproject/obs-studio/archive/refs/tags/$obsVersion.zip"; Sha = '2c8427c10b55ac6d68008df2e9a3e82f4647aaad18f105e30d4713c2de678ccf'; Dir = 'src' },
  @{ Name = "windows-deps-$depsVersion-x64.zip"; Url = "https://github.com/obsproject/obs-deps/releases/download/$depsVersion/windows-deps-$depsVersion-x64.zip"; Sha = 'c8c642c1070dc31ce9a0f1e4cef5bb992f4bff4882255788b5da12129e85caa7'; Dir = 'deps' },
  @{ Name = "windows-deps-qt6-$depsVersion-x64.zip"; Url = "https://github.com/obsproject/obs-deps/releases/download/$depsVersion/windows-deps-qt6-$depsVersion-x64.zip"; Sha = '0e76bf0555dd5382838850b748d3dcfab44a1e1058441309ab54e1a65b156d0a'; Dir = 'qt6' }
)

New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path
foreach ($f in $files) {
  $zip = Join-Path $Dest $f.Name
  if (-not (Test-Path $zip) -and $Cache -and (Test-Path (Join-Path $Cache $f.Name))) {
    Copy-Item (Join-Path $Cache $f.Name) $zip
  }
  if (-not (Test-Path $zip)) {
    Write-Host "Downloading $($f.Url)"
    Invoke-WebRequest -Uri $f.Url -OutFile $zip -UseBasicParsing
  }
  $sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  if ($sha -ne $f.Sha) {
    Remove-Item -Force $zip
    throw "SHA-256 mismatch for $($f.Name): $sha"
  }
  $out = Join-Path $Dest $f.Dir
  if (-not (Test-Path $out)) {
    Write-Host "Extracting $($f.Name)"
    # The source zip has a top folder; the dependency zips do not, so they get their own.
    $into = if ($f.Dir -eq 'src') { $Dest } else { New-Item -ItemType Directory -Force $out | Select-Object -ExpandProperty FullName }
    # cmake's extractor copes with the symlinks in the OBS source zip (Windows tar does not)
    Push-Location $into
    cmake -E tar xf $zip
    $code = $LASTEXITCODE
    Pop-Location
    if ($code) { throw "extract failed: $($f.Name)" }
    if ($f.Dir -eq 'src') {
      Rename-Item (Join-Path $Dest "obs-studio-$obsVersion") 'src'
    }
  }
}

$src = Join-Path $Dest 'src'
$build = Join-Path $Dest 'obs-build'
$prefix = "$(Join-Path $Dest 'deps');$(Join-Path $Dest 'qt6')"
Write-Host 'Configuring libobs'
cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 `
  -DOBS_CMAKE_VERSION:STRING=3.0.0 -DENABLE_PLUGINS:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF `
  "-DOBS_VERSION_OVERRIDE:STRING=$obsVersion" "-DCMAKE_PREFIX_PATH=$prefix" | Out-Null
if ($LASTEXITCODE) { throw 'configure failed' }
Write-Host 'Building libobs and obs-frontend-api'
cmake --build $build --target obs-frontend-api --config Release --parallel | Out-Null
if ($LASTEXITCODE) { throw 'build failed' }
cmake --install $build --component Development --config Release --prefix $Dest | Out-Null
if ($LASTEXITCODE) { throw 'install failed' }
Write-Host "OBS SDK ready: -DCMAKE_PREFIX_PATH=`"$Dest;$(Join-Path $Dest 'qt6');$(Join-Path $Dest 'deps')`""
