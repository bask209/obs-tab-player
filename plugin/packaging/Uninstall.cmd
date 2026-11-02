@echo off
rem SPDX-License-Identifier: Apache-2.0
rem Removes OBS Tab Player. Your settings in %APPDATA%\obs-studio\plugin_config\obs-tab-player are kept.
set "DEST=%ProgramData%\obs-studio\plugins\obs-tab-player"
tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS Studio is open. Please close OBS, then press any key to continue.
  pause >nul
)
rmdir /S /Q "%DEST%"
if exist "%DEST%" (
  echo Could not remove %DEST%. Try right-click ^> "Run as administrator".
) else (
  echo OBS Tab Player was removed.
)
pause
