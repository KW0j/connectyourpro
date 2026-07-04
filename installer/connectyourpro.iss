; connectyourpro — Inno Setup installer script
; Bundles the app + ViGEmBus driver (installed only when missing).
; Build:  ISCC.exe connectyourpro.iss   (run from this directory)
; Expects ..\build\testapp.exe to exist (build via ..\build_release.bat)
; and redist\ViGEmBus_Setup.exe (ViGEmBus 1.22.0 from nefarius/ViGEmBus releases).

#define MyAppName "connectyourpro"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "connectyourpro"
#define MyAppURL "https://connectyour.pro"
#define MyAppExeName "connectyourpro.exe"

[Setup]
AppId={{8E1C7A52-6B0F-4E1B-9A57-3D92F4B7A1C6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=connectyourpro-setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\testapp.exe"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "redist\ViGEmBus_Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Install the ViGEmBus driver only when it is not present yet
Filename: "{tmp}\ViGEmBus_Setup.exe"; Parameters: "/passive /norestart"; \
    StatusMsg: "Installing ViGEmBus driver (virtual gamepad)..."; \
    Check: not ViGEmBusInstalled; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
    Flags: nowait postinstall skipifsilent

[Code]
function ViGEmBusInstalled: Boolean;
begin
  // The ViGEmBus kernel service registers here once the driver is installed
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;
