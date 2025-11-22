#define MyAppName "BlockWin"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "BlockWin"
#define MyAppExeName "BlockWin.exe"

[Setup]
AppId={{B3F5A3C4-2D7E-4C0D-9A11-5E9E6B7C8A21}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=BlockWin-Setup
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
SetupIconFile=..\BlockWin\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
PrivilegesRequiredOverridesAllowed=dialog
PrivilegesRequired=admin
UsePreviousAppDir=no
CloseApplications=yes
AppMutex=BlockWin_SingleInstance_Mutex
DisableDirPage=no
AllowRootDirectory=yes
AllowUNCPath=yes

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
;Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительные задачи:"; Flags: unchecked
Name: "autorun"; Description: "Запускать при входе в Windows"; GroupDescription: "Дополнительные задачи:";

[Files]
Source: "..\\x64\\Debug\\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\BlockWin\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico"; DestDir: "{app}"; Flags: ignoreversion
;Source: "..\BlockWin\toggles.dat"; DestDir: "{app}"; Flags: onlyifdoesntexist

[Icons]
; Иконка запуска приложения
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--activate"; WorkingDir: "{app}"; IconFilename: "{app}\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico"; AppUserModelID: "BlockWin.BlockWinApp"

; Автозапуск в фоне (по задаче)
Name: "{userstartup}\{#MyAppName}.lnk"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--background"; WorkingDir: "{app}"; IconFilename: "{app}\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico"; Tasks: autorun

; Иконка деинсталлятора
Name: "{autoprograms}\Удалить {#MyAppName}"; Filename: "{uninstallexe}"; IconFilename: "{app}\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico"; AppUserModelID: "BlockWin.BlockWinApp.Uninstall"

;Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Запустить {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\toggles.dat"
Type: filesandordirs; Name: "{app}"

[Dirs]
Name: "{app}"; Permissions: users-modify

[Code]
const
  WM_COMMAND = $0111;
  ID_TRAY_EXIT = 40002;
  APP_CLASS = 'BlockWinApp';
  APP_TITLE = 'BlockWin — Киберпанк режим';

function FindWindow(lpClassName, lpWindowName: string): HWND;
  external 'FindWindowW@user32.dll stdcall';

function PostMessage(hWnd: HWND; Msg, wParam, lParam: Longint): Longint;
  external 'PostMessageW@user32.dll stdcall';

procedure CloseRunningApp();
var
  hwnd: HWND;
  i: Integer;
begin
  // Ищем по классу и известному заголовку окна
  hwnd := FindWindow(APP_CLASS, APP_TITLE);
  // Если не нашли, пробуем только по классу
  if hwnd = 0 then
    hwnd := FindWindow(APP_CLASS, '');
  if hwnd <> 0 then begin
    PostMessage(hwnd, WM_COMMAND, ID_TRAY_EXIT, 0);
    // Ждём до 5 секунд, пока окно завершится
    for i := 1 to 50 do begin
      Sleep(100);
      if FindWindow(APP_CLASS, APP_TITLE) = 0 then Break;
    end;
  end;
end;
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    CloseRunningApp();
  end;
end;
