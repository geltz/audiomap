[Setup]
AppId={{E4F14619-EDF4-4CCB-BBF9-251730C00B4A}
AppName=audiomap
AppVersion=1.1
AppPublisher=geltz
DefaultDirName={autopf}\audiomap
UsePreviousAppDir=no
DefaultGroupName=audiomap
SetupIconFile=audiomap.ico
UninstallDisplayIcon={app}\audiomap.ico
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=.
OutputBaseFilename=audiomap_setup_1.1
WizardStyle=modern
ChangesAssociations=no
DirExistsWarning=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "audiomap.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "audiomap.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\audiomap"; Filename: "{app}\audiomap.exe"; IconFilename: "{app}\audiomap.ico"
Name: "{group}\{cm:UninstallProgram,audiomap}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\audiomap"; Filename: "{app}\audiomap.exe"; IconFilename: "{app}\audiomap.ico"; Tasks: desktopicon

[UninstallDelete]
Type: files; Name: "{app}\audiomap.exe"
Type: files; Name: "{app}\audiomap.ico"
Type: files; Name: "{app}\unins*.exe"
Type: files; Name: "{app}\unins*.dat"

[Run]
Filename: "{app}\audiomap.exe"; Description: "{cm:LaunchProgram,audiomap}"; Flags: nowait postinstall skipifsilent