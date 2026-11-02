@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Installs OBS Tab Player into the standard OBS plugin folder. No administrator rights needed:
rem Windows lets every user add folders under C:\ProgramData.
setlocal
set "SRC=%~dp0obs-tab-player"
set "DEST=%ProgramData%\obs-studio\plugins\obs-tab-player"

if not exist "%SRC%\bin\64bit\obs-tab-player.dll" (
  echo Please extract the whole ZIP first, then run Install.cmd from the extracted folder.
  pause
  exit /b 1
)

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS Studio is open. Please close OBS, then press any key to continue.
  pause >nul
)

robocopy "%SRC%" "%DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
if %errorlevel% GEQ 8 (
  echo Installation failed: could not write to %DEST%
  echo If an older copy was installed by an administrator, right-click Install.cmd and choose
  echo "Run as administrator" once.
  pause
  exit /b 1
)

echo.
echo  OBS Tab Player is installed.
echo.
echo  Start OBS: the "Tab Player" panel opens on the right.
echo  1. Click "Choose your tabs folder"
echo  2. Click "Add Tab Player to this scene"
echo  Then pick a tab and press Play.
echo.
pause
