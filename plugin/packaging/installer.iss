; SPDX-License-Identifier: Apache-2.0
; Inno Setup script for obs-tab-player-<version>-windows-x64-setup.exe
;   ISCC.exe /DAppVersion=0.2.0 /DStageDir=..\..\dist\stage plugin\packaging\installer.iss
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\dist\stage"
#endif

[Setup]
AppId={{3F6C2D61-8E1B-4C5A-9D27-6B1E0A4C7F22}
AppName=OBS Tab Player
AppVersion={#AppVersion}
AppPublisher=bask209
AppPublisherURL=https://bask209.github.io/obs-tab-player/
AppSupportURL=https://github.com/bask209/obs-tab-player/issues
DefaultDirName={commonappdata}\obs-studio\plugins\obs-tab-player
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir=..\..\dist
OutputBaseFilename=obs-tab-player-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS Tab Player
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#StageDir}\obs-tab-player\bin\64bit\obs-tab-player.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StageDir}\obs-tab-player\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Messages]
FinishedLabel=OBS Tab Player is installed.%n%nStart OBS Studio: the Tab Player panel opens on the right. Click "Choose your tabs folder", then "Add Tab Player to this scene", pick a tab and press Play.

[Code]
// Only Administrators and SYSTEM may modify the DLL that OBS loads.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
    Exec(ExpandConstant('{sys}\icacls.exe'),
      '"' + ExpandConstant('{app}') + '" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
