[Setup]
AppName=BlockWin
AppVersion=1.0
DefaultDirName={pf}\BlockWin
DefaultGroupName=BlockWin
OutputBaseFilename=BlockWinSetup
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin

[Files]
; Убедитесь, что BlockWin.exe реально существует в D:\script
Source: "D:\script\BlockWin.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\BlockWin"; Filename: "{app}\BlockWin.exe"; WorkingDir: "{app}"
Name: "{commondesktop}\BlockWin"; Filename: "{app}\BlockWin.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительно"; Flags: unchecked
Name: "autostart"; Description: "Запускать BlockWin при старте Windows"; GroupDescription: "Дополнительно"; Flags: unchecked

[Registry]
; Добавляем в автозагрузку через реестр, если выбрана опция автозапуска
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "BlockWin"; ValueData: """{app}\BlockWin.exe"""; Tasks: autostart; Flags: uninsdeletevalue
