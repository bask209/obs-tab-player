; SPDX-License-Identifier: Apache-2.0
; Inno Setup script: builds obs-tab-player-<version>-windows-x64-setup.exe from the staged
; install tree (tools\package-plugin.ps1 creates it).
;   ISCC.exe /DAppVersion=0.4.0 /DStageDir=..\..\dist\stage plugin\packaging\installer.iss
#ifndef AppVersion
  #define AppVersion "0.4.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\dist\stage"
#endif

[Setup]
AppId={{3F6C2D61-8E1B-4C5A-9D27-6B1E0A4C7F22}
AppName=OBS Tab Player
AppVersion={#AppVersion}
AppVerName=OBS Tab Player {#AppVersion}
AppPublisher=bask209
AppPublisherURL=https://bask209.github.io/obs-tab-player/
AppSupportURL=https://github.com/bask209/obs-tab-player/issues
; Standard OBS plugin location (loaded by OBS 28+ for all users).
DefaultDirName={commonappdata}\obs-studio\plugins\obs-tab-player
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
; No administrator rights needed: every user may add folders under C:\ProgramData, which is
; the only place OBS on Windows loads plugins from (besides its own folder). /ALLUSERS still
; works from the command line and then also locks the folder to administrators.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir=..\..\dist
OutputBaseFilename=obs-tab-player-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS Tab Player
; Updating while OBS has the DLL loaded: offer to close OBS via Restart Manager.
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#StageDir}\obs-tab-player\bin\64bit\obs-tab-player.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StageDir}\obs-tab-player\bin\64bit\obs-tab-player.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\obs-tab-player\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs
Source: "{#StageDir}\demo-tabs\*"; DestDir: "{app}\demo-tabs"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Messages]
FinishedLabel=OBS Tab Player is installed.%n%nStart (or restart) OBS Studio: the Tab Player panel opens on the right. Click "Choose your tabs folder", then "Add Tab Player to this scene", pick a tab and press Play.%n%nThe guitar and the metronome get their own faders in the Audio Mixer.

[Code]
// Admin installs (/ALLUSERS) only: lock the plugin folder down: only Administrators and SYSTEM may change the DLL that OBS
// loads; everyone else can only read and execute it. (By default ProgramData lets any
// user create files in subfolders.)
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and IsAdminInstallMode() then
    Exec(ExpandConstant('{sys}\icacls.exe'),
      '"' + ExpandConstant('{app}') + '" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function ObsInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SOFTWARE\OBS Studio') or RegKeyExists(HKLM64, 'SOFTWARE\OBS Studio')
    or FileExists(ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe'));
end;

function InitializeSetup(): Boolean;
var
  Probe: String;
begin
  Result := True;
  // An older copy installed by an administrator may be locked against changes by normal users.
  Probe := ExpandConstant('{commonappdata}\obs-studio\plugins\obs-tab-player\.write-test');
  if not IsAdminInstallMode() and DirExists(ExtractFileDir(Probe)) then
  begin
    if not SaveStringToFile(Probe, 'ok', False) then
    begin
      MsgBox('An older OBS Tab Player was installed by an administrator, so it can only be updated as administrator this one time.'
        + #13#10#13#10 + 'Right-click the installer and choose "Run as administrator".', mbInformation, MB_OK);
      Result := False;
      exit;
    end;
    DeleteFile(Probe);
  end;
  if not ObsInstalled() and not WizardSilent() then
    Result := MsgBox('OBS Studio was not found on this PC. OBS Tab Player is a plugin for OBS Studio 30 or newer.'
      + #13#10#13#10 + 'Install the plugin anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    ConfigDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\obs-tab-player');
    if DirExists(ConfigDir) and not UninstallSilent() then
      if MsgBox('Also delete your Tab Player settings (tabs folder, tempo, look, hotkeys)?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ConfigDir, True, True, True);
  end;
end;
