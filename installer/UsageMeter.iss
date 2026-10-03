; Inno Setup script. Build with: ISCC installer\UsageMeter.iss
#define AppName "UsageMeter"
#define AppVersion "1.0.0"
#define AppPublisher "CodingIsCoolFr"
#define AppURL "https://github.com/CodingIsCoolFr/UsageMeter"

[Setup]
AppId={{A7E3C1B2-4F58-4A1E-9C0D-6B2E8F1A4D77}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=UsageMeter-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=
UninstallDisplayIcon={app}\UsageMeter.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Files]
Source: "..\dist\windows\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\UsageMeter.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\UsageMeter.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Run]
Filename: "{app}\UsageMeter.exe"; Description: "Launch UsageMeter"; Flags: nowait postinstall skipifsilent
