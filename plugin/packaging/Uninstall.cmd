@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Removes OBS Tab Player. Your settings in %APPDATA%\obs-studio\plugin_config\obs-tab-player are kept.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)
rmdir /S /Q "%ProgramData%\obs-studio\plugins\obs-tab-player"
echo OBS Tab Player was removed.
pause
