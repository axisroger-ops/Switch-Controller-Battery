; installer.iss - Inno Setup script for Switch Controller Battery
#define MyAppName "Switch Controller Battery"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Open Source"
#define MyAppURL "https://github.com/axisroger-ops/Switch-Controller-Battery"
#define MyAppExeName "SwitchControllerBattery.exe"

[Setup]
AppId={{9F57E200-9B21-4E89-B09A-SwitchBattery}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DisableDirPage=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=SwitchControllerBattery-Setup-v1.0.0
SetupIconFile=app.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
UsedUserAreasWarning=no
CloseApplications=force

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Files]
Source: "SwitchControllerBattery.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "SwitchControllerBatteryHook.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "uninstall.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "app.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"

[Registry]
; Démarrage automatique en tâche de fond avec Windows
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "SwitchControllerBattery"; ValueData: """{app}\{#MyAppExeName}"" --background"; Flags: uninsdeletevalue

[Run]
; Appliquer les permissions AppContainer nécessaires pour Windows 11
Filename: "icacls.exe"; Parameters: """{app}"" /grant *S-1-15-2-1:(OI)(CI)(RX) /Q"; Flags: runhidden
Filename: "icacls.exe"; Parameters: """{app}\SwitchControllerBatteryHook.dll"" /grant *S-1-15-2-1:(RX) /Q"; Flags: runhidden
; Lancement de la configuration après l'installation
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Arrêt propre des processus avant désinstallation
Filename: "taskkill.exe"; Parameters: "/F /IM {#MyAppExeName}"; Flags: runhidden
Filename: "taskkill.exe"; Parameters: "/F /IM ShellHost.exe"; Flags: runhidden
Filename: "taskkill.exe"; Parameters: "/F /IM SystemSettings.exe"; Flags: runhidden
