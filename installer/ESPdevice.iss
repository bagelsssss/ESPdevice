; ESPdevice installer (Inno Setup 6). Built by CI: iscc /DMyAppVersion=1.0.0 installer\ESPdevice.iss
; Installs per-user (no admin prompt) because ESPdevice keeps its config\ folder
; next to the exe, and Program Files is not writable without admin.

#ifndef MyAppVersion
  #define MyAppVersion "dev"
#endif

[Setup]
AppId={{6E0B2F4A-5B1D-4C8E-9A57-3D1E7C2A9F10}
AppName=ESPdevice
AppVersion={#MyAppVersion}
AppPublisher=bagelings
AppPublisherURL=https://github.com/bagelsssss/ESPdevice
AppSupportURL=https://github.com/bagelsssss/ESPdevice/issues
DefaultDirName={localappdata}\ESPdevice
DefaultGroupName=ESPdevice
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=ESPdevice-Setup
SetupIconFile=..\app\espdevice.ico
UninstallDisplayIcon={app}\ESPdevice.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Tasks]
Name: "autostart"; Description: "Start ESPdevice when I sign in to Windows"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Options:"; Flags: unchecked

[Files]
Source: "..\app\ESPdevice.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ESPdevice"; Filename: "{app}\ESPdevice.exe"
Name: "{autoprograms}\ESPdevice settings (browser)"; Filename: "http://localhost:9191"
Name: "{autodesktop}\ESPdevice"; Filename: "{app}\ESPdevice.exe"; Tasks: desktopicon

[Registry]
; Per-user autostart; removed again on uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ESPdevice"; ValueData: """{app}\ESPdevice.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\ESPdevice.exe"; Description: "Start ESPdevice now"; Flags: nowait postinstall skipifsilent

[Code]
// ESPdevice is a tray app with no window, so Inno cannot ask it to close. Stop it
// before files are replaced (upgrade) or removed (uninstall).
procedure StopESPdevice();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ESPdevice.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(400);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopESPdevice();
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopESPdevice();
  if CurUninstallStep = usPostUninstall then
    if DirExists(ExpandConstant('{app}\config')) then
      if MsgBox('Also delete your ESPdevice settings (profiles, themes, logo)?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{app}'), True, True, True);
end;
