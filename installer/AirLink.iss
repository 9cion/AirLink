#define AppVersion "0.1.0"
[Setup]
AppId={{3585975D-C6D4-49E2-AB16-76A79F0FB9B3}
AppName=AirLink
AppVersion={#AppVersion}
AppPublisher=AirLink
DefaultDirName={localappdata}\Programs\AirLink
DefaultGroupName=AirLink
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
OutputDir=..\dist
OutputBaseFilename=AirLink-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\AirLink.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\release\AirLink.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\AirLink.Transport.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\images\logo.png"; DestDir: "{app}\docs\images"; Flags: ignoreversion
Source: "..\docs\images\light-mode.png"; DestDir: "{app}\docs\images"; Flags: ignoreversion
Source: "..\docs\images\dark-mode.png"; DestDir: "{app}\docs\images"; Flags: ignoreversion

[Icons]
Name: "{group}\AirLink"; Filename: "{app}\AirLink.exe"
Name: "{group}\AirLink deinstallieren"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\AirLink.exe"; Description: "AirLink starten"; Flags: nowait postinstall skipifsilent unchecked

