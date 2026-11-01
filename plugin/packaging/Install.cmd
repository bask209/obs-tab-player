@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Installs OBS Tab Player for all users (standard OBS 30+ plugin folder). Asks for admin rights.
setlocal
set "SRC=%~dp0obs-tab-player"
set "DEST=%ProgramData%\obs-studio\plugins\obs-tab-player"

if not exist "%SRC%\bin\64bit\obs-tab-player.dll" (
  echo Please extract the whole ZIP first, then run Install.cmd from the extracted folder.
  pause
  exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS Studio is open. Please close OBS, then press any key to continue.
  pause >nul
)

robocopy "%SRC%" "%DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
if %errorlevel% GEQ 8 (
  echo Installation failed.
  pause
  exit /b 1
)
rem Only administrators may change the files OBS loads.
icacls "%DEST%" /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-545:(OI)(CI)RX" >nul

echo.
echo  OBS Tab Player is installed.
echo.
echo  Start OBS: the "Tab Player" panel opens on the right.
echo  1. Click "Choose your tabs folder"
echo  2. Click "Add Tab Player to this scene"
echo  Then pick a tab and press Play.
echo.
pause
