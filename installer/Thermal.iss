#ifndef AppVersion
  #error AppVersion must be supplied by build-installer.ps1
#endif
[Setup]
AppId={{1F59D94C-25D6-47EC-B4D2-BA1F08F85261}
AppName=Thermal
AppVersion={#AppVersion}
AppPublisher=Thermal
AppPublisherURL=https://github.com/igorsharendo-ctrl/thermal-overlay
DefaultDirName={localappdata}\Programs\Thermal
DefaultGroupName=Thermal
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir=..\build\release
OutputBaseFilename=Thermal-Setup-{#AppVersion}-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Thermal.exe
AppMutex=Local\ThermalOverlay
CloseApplications=yes
RestartApplications=no
InfoBeforeFile=install-info.txt
VersionInfoVersion={#AppVersion}.0

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; Flags: unchecked

[Files]
Source: "..\build\Thermal.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\support\*"; DestDir: "{app}\support"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\VERSION"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\Thermal"; Filename: "{app}\Thermal.exe"
Name: "{userdesktop}\Thermal"; Filename: "{app}\Thermal.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Thermal.exe"; Description: "Запустить Thermal"; Flags: nowait postinstall skipifsilent

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'ThermalOverlay', Command) then
      if CompareText(Command, '"' + ExpandConstant('{app}\Thermal.exe') + '"') = 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'ThermalOverlay');
end;
