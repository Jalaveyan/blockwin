#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propvarutil.h>
#include <propkey.h>
#include <shlwapi.h>
#include <guiddef.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <atomic>
#include <cmath>
#include <memory>
#include <cwctype>
#include "Resource.h"

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"propsys.lib")
#pragma comment(lib,"shlwapi.lib")

using namespace Gdiplus;
using namespace std;
using Microsoft::WRL::ComPtr;

HINSTANCE hInst;
HWND hwndMain = NULL;
HWND hwndTray = NULL;
HHOOK hHook = NULL;
NOTIFYICONDATA nid = {};
ULONG_PTR gdiplusToken = 0;
HBRUSH gBgBrush = NULL;
HICON gIconBig = NULL;
HICON gIconSmall = NULL;
HICON gIconTray = NULL;
HMENU gTrayMenu = NULL;
HANDLE gSingleInstanceMutex = NULL;
bool gTrayAdded = false;
UINT gMsgTaskbarCreated = 0;
UINT gMsgActivate = 0;
// Устойчивый GUID для иконки трея (чтобы не плодились дубликаты)
static const GUID kTrayGuid = { 0x7b2f6e3a, 0x6e9a, 0x4b2d, { 0x9c, 0x41, 0x6a, 0x8e, 0x3b, 0x2f, 0x71, 0x11 } };

#define ID_TRAY_OPEN  40001
#define ID_TRAY_EXIT  40002

// Единый цвет фона (близкий к образцу). Меняйте эти три байта — оттенок обновится везде.
static const BYTE kBgR = 26;
static const BYTE kBgG = 27;
static const BYTE kBgB = 40;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20 19
#endif

// Окно только для трея (message-only)
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) return 0;
    if (msg == WM_USER + 1) {
        // проксируем в главное окно
        if (hwndMain) return SendMessage(hwndMain, msg, wParam, lParam);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
static void EnableDarkTitleBar(HWND hwnd, bool enable) {
    if (!hwnd) return;
    BOOL val = enable ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)DWMWA_USE_IMMERSIVE_DARK_MODE, &val, sizeof(val)))) {
        DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20, &val, sizeof(val));
    }
}

static void SetAppUserModelForWindow(HWND hwnd) {
    if (!hwnd) return;
    SetCurrentProcessExplicitAppUserModelID(L"BlockWin.BlockWinApp");
    IPropertyStore* pps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&pps)))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);

        // AppID
        if (SUCCEEDED(InitPropVariantFromString(L"BlockWin.BlockWinApp", &pv))) {
            pps->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }

        // Relaunch command/display/icon
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (SUCCEEDED(InitPropVariantFromString(exe, &pv))) {
            pps->SetValue(PKEY_AppUserModel_RelaunchCommand, pv);
            PropVariantClear(&pv);
        }
        if (SUCCEEDED(InitPropVariantFromString(L"BlockWin — Киберпанк режим", &pv))) {
            pps->SetValue(PKEY_AppUserModel_RelaunchDisplayNameResource, pv);
            PropVariantClear(&pv);
        }
        if (SUCCEEDED(InitPropVariantFromString(exe, &pv))) {
            pps->SetValue(PKEY_AppUserModel_RelaunchIconResource, pv);
            PropVariantClear(&pv);
        }

        pps->Commit();
        pps->Release();
    }
}

static void EnsurePinnedTaskbarShortcut() {
    wchar_t appdata[MAX_PATH] = {0};
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) return;
    wstring pinnedDir = wstring(appdata) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
    CreateDirectoryW(pinnedDir.c_str(), NULL);

    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    // Обновляем все ярлыки на BlockWin.exe в папке закреплений
    WIN32_FIND_DATAW fd; HANDLE hFind = INVALID_HANDLE_VALUE;
    wstring glob = pinnedDir + L"\\*.lnk";
    hFind = FindFirstFileW(glob.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            wstring lnkPath = pinnedDir + L"\\" + fd.cFileName;
            ComPtr<IShellLinkW> sl;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
                ComPtr<IPersistFile> pf;
                if (SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Load(lnkPath.c_str(), STGM_READWRITE))) {
                    wchar_t target[MAX_PATH] = {0};
                    if (SUCCEEDED(sl->GetPath(target, MAX_PATH, NULL, SLGP_RAWPATH))) {
                        if (target[0] && _wcsicmp(PathFindFileNameW(target), L"BlockWin.exe") == 0) {
                            sl->SetPath(exe);
                            sl->SetArguments(L"--activate");
                            sl->SetIconLocation(exe, 0);
                            sl->SetWorkingDirectory(PathRemoveFileSpecW(exe) ? exe : L"");
                            ComPtr<IPropertyStore> pps;
                            if (SUCCEEDED(sl.As(&pps))) {
                                PROPVARIANT pv; PropVariantInit(&pv);
                                if (SUCCEEDED(InitPropVariantFromString(L"BlockWin.BlockWinApp", &pv))) {
                                    pps->SetValue(PKEY_AppUserModel_ID, pv); PropVariantClear(&pv);
                                }
                                pps->Commit();
                            }
                            pf->Save(lnkPath.c_str(), TRUE);
                        }
                    }
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    // Создаём/перезаписываем наш основной ярлык
    wstring mainLnk = pinnedDir + L"\\BlockWin.lnk";
    ComPtr<IShellLinkW> sl;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(exe);
        sl->SetArguments(L"--activate");
        sl->SetIconLocation(exe, 0);
        sl->SetWorkingDirectory(PathRemoveFileSpecW(exe) ? exe : L"");
        ComPtr<IPropertyStore> pps;
        if (SUCCEEDED(sl.As(&pps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(InitPropVariantFromString(L"BlockWin.BlockWinApp", &pv))) {
                pps->SetValue(PKEY_AppUserModel_ID, pv); PropVariantClear(&pv);
            }
            pps->Commit();
        }
        ComPtr<IPersistFile> pf; if (SUCCEEDED(sl.As(&pf))) pf->Save(mainLnk.c_str(), TRUE);
    }
}

// Обновляет AUMID у уже закреплённых ярлыков BlockWin.exe, не создавая новых
static void AlignPinnedTaskbarShortcutsAUMID() {
    wchar_t appdata[MAX_PATH] = {0};
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) return;
    wstring pinnedDir = wstring(appdata) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
    CreateDirectoryW(pinnedDir.c_str(), NULL);

    wstring glob = pinnedDir + L"\\*.lnk";
    WIN32_FIND_DATAW fd; HANDLE hFind = FindFirstFileW(glob.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        wstring lnkPath = pinnedDir + L"\\" + fd.cFileName;
        IShellLinkW* pSL = nullptr; IPersistFile* pPF = nullptr; IPropertyStore* pPS = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pSL)))) {
            if (SUCCEEDED(pSL->QueryInterface(IID_PPV_ARGS(&pPF))) && SUCCEEDED(pPF->Load(lnkPath.c_str(), STGM_READWRITE))) {
                wchar_t target[MAX_PATH] = {0};
                if (SUCCEEDED(pSL->GetPath(target, MAX_PATH, NULL, SLGP_RAWPATH))) {
                    if (target[0] && _wcsicmp(PathFindFileNameW(target), L"BlockWin.exe") == 0) {
                        if (SUCCEEDED(pSL->QueryInterface(IID_PPV_ARGS(&pPS)))) {
                            PROPVARIANT pv; PropVariantInit(&pv);
                            if (SUCCEEDED(InitPropVariantFromString(L"BlockWin.BlockWinApp", &pv))) {
                                pPS->SetValue(PKEY_AppUserModel_ID, pv);
                                PropVariantClear(&pv);
                            }
                            pPS->Commit();
                        }
                        pPF->Save(lnkPath.c_str(), TRUE);
                    }
                }
            }
        }
        if (pPS) pPS->Release();
        if (pPF) pPF->Release();
        if (pSL) pSL->Release();
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

// Анимации / состояние
atomic<bool> gameModeActive(false);
float gameAnimLevel = 0.0f;
float neonPulse = 0.0f;
float pulseSpeed = 0.06f;

// Toggle структура
struct Toggle {
    atomic<bool> enabled;
    float animLevel;
    wstring label;

    Toggle(const wstring& l, bool e = false) : enabled(e), animLevel(e ? 1.0f : 0.0f), label(l) {}
    Toggle(const Toggle&) = delete;
    Toggle& operator=(const Toggle&) = delete;
};
vector<unique_ptr<Toggle>> toggles;
// Allowlist исполняемых файлов игр (имена .exe в нижнем регистре)
vector<wstring> gGameExeAllowlist;

static bool IsNonGameCommonProcess(const wstring& exeLower) {
    return
        // Browsers
        exeLower == L"chrome.exe" || exeLower == L"msedge.exe" || exeLower == L"firefox.exe" ||
        exeLower == L"opera.exe" || exeLower == L"opera_gx.exe" || exeLower == L"vivaldi.exe" ||
        exeLower == L"yandex.exe" || exeLower == L"brave.exe" || exeLower == L"iexplore.exe" ||
        // Messengers / Conferencing
        exeLower == L"discord.exe" || exeLower == L"teams.exe" || exeLower == L"ms-teams.exe" ||
        exeLower == L"slack.exe" || exeLower == L"zoom.exe" || exeLower == L"skype.exe" ||
        exeLower == L"telegram.exe" || exeLower == L"telegramdesktop.exe" || exeLower == L"whatsapp.exe" ||
        exeLower == L"viber.exe" ||
        // Dev / IDE / Editors
        exeLower == L"devenv.exe" || exeLower == L"code.exe" || exeLower == L"code - insiders.exe" ||
        exeLower == L"pycharm64.exe" || exeLower == L"idea64.exe" || exeLower == L"clion64.exe" ||
        exeLower == L"goland64.exe" || exeLower == L"webstorm64.exe" || exeLower == L"rider64.exe" ||
        exeLower == L"studio64.exe" || exeLower == L"notepad++.exe" || exeLower == L"sublime_text.exe" ||
        exeLower == L"atom.exe" || exeLower == L"qtcreator.exe" ||
        // Media / Players / Music
        exeLower == L"vlc.exe" || exeLower == L"mpc-hc64.exe" || exeLower == L"mpc-hc.exe" ||
        exeLower == L"mpc-be64.exe" || exeLower == L"mpc-be.exe" || exeLower == L"potplayer64.exe" ||
        exeLower == L"potplayer.exe" || exeLower == L"kmplayer.exe" || exeLower == L"mpv.exe" ||
        exeLower == L"spotify.exe" || exeLower == L"itunes.exe" || exeLower == L"foobar2000.exe" ||
        exeLower == L"winamp.exe" ||
        // Adobe / Creative
        exeLower == L"adobe premiere pro.exe" || exeLower == L"premierepro.exe" ||
        exeLower == L"afterfx.exe" || exeLower == L"photoshop.exe" || exeLower == L"illustrator.exe" ||
        exeLower == L"lightroom.exe" || exeLower == L"blender.exe" ||
        // Launchers / clients (not games)
        exeLower == L"steam.exe" || exeLower == L"steamwebhelper.exe" ||
        exeLower == L"epicgameslauncher.exe" || exeLower == L"origin.exe" || exeLower == L"eadesktop.exe" ||
        exeLower == L"ealink.exe" || exeLower == L"uplay.exe" || exeLower == L"ubisoftconnect.exe" ||
        exeLower == L"gog galaxy.exe" || exeLower == L"galaxyclient.exe" || exeLower == L"galaxyclientservice.exe" ||
        exeLower == L"riotclientservices.exe" || exeLower == L"riotclientux.exe" || exeLower == L"riotclientuxrender.exe" ||
        exeLower == L"rockstar games launcher.exe" ||
        // Streaming / overlays / system helpers
        exeLower == L"obs64.exe" || exeLower == L"obs32.exe" || exeLower == L"nvidia share.exe" ||
        exeLower == L"nvcontainer.exe" || exeLower == L"nvidia broadcast.exe" || exeLower == L"amdsoftware.exe" ||
        exeLower == L"radeonsoftware.exe" || exeLower == L"gamebar.exe" || exeLower == L"gamebarpresencewriter.exe" ||
        exeLower == L"gamingservicesui.exe" || exeLower == L"xboxappservices.exe" || exeLower == L"xboxapp.exe" ||
        // Office / PDF / Mail
        exeLower == L"winword.exe" || exeLower == L"excel.exe" || exeLower == L"powerpnt.exe" ||
        exeLower == L"onenote.exe" || exeLower == L"outlook.exe" || exeLower == L"acrobat.exe" ||
        exeLower == L"foxitreader.exe" ||
        // Shell / utilities
        exeLower == L"explorer.exe" || exeLower == L"windowsterminal.exe" || exeLower == L"conhost.exe" ||
        exeLower == L"cmd.exe" || exeLower == L"powershell.exe" || exeLower == L"pwsh.exe";
}

static bool ProcessUsesGraphicsRuntime(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
    bool has = false;
    if (Module32FirstW(snap, &me)) {
        do {
            const wchar_t* modName = me.szModule;
            wstring lower = modName ? modName : L"";
            for (wchar_t& ch : lower) ch = (wchar_t)towlower(ch);
            if (lower == L"dxgi.dll" || lower == L"d3d11.dll" || lower == L"d3d12.dll" || lower == L"opengl32.dll" || lower == L"vulkan-1.dll") {
                has = true; break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return has;
}

static bool IsPathInKnownGameDirs(const wstring& exePathLower) {
    return exePathLower.find(L"\\steam\\steamapps\\common\\") != wstring::npos ||
           exePathLower.find(L"\\steamlibrary\\steamapps\\common\\") != wstring::npos ||
           exePathLower.find(L"\\epic games\\") != wstring::npos ||
           exePathLower.find(L"\\riot games\\") != wstring::npos ||
           exePathLower.find(L"\\origin games\\") != wstring::npos ||
           exePathLower.find(L"\\ea games\\") != wstring::npos ||
           exePathLower.find(L"\\ubisoft game launcher\\games\\") != wstring::npos ||
           exePathLower.find(L"\\battle.net\\") != wstring::npos ||
           exePathLower.find(L"\\wargaming.net\\") != wstring::npos ||
           exePathLower.find(L"\\genshin impact game\\") != wstring::npos ||
           exePathLower.find(L"\\riot client\\") != wstring::npos ||
           exePathLower.find(L"\\roblox\\versions\\") != wstring::npos ||
           exePathLower.find(L"\\xboxgames\\") != wstring::npos ||
           exePathLower.find(L"\\windowsapps\\") != wstring::npos;
}

static bool ProcessHasGameSignatureModules(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
    bool has = false;
    if (Module32FirstW(snap, &me)) {
        do {
            const wchar_t* modName = me.szModule;
            wstring lower = modName ? modName : L"";
            for (wchar_t& ch : lower) ch = (wchar_t)towlower(ch);
            if (lower.find(L"xinput1_") != wstring::npos || lower == L"xinput9_1_0.dll" ||
                lower == L"steam_api.dll" || lower == L"steam_api64.dll" ||
                lower == L"unityplayer.dll" ||
                lower.find(L"eossdk") != wstring::npos ||
                lower.find(L"easyanticheat") != wstring::npos ||
                lower.find(L"beclient") != wstring::npos || lower.find(L"bedaisy") != wstring::npos) {
                has = true; break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return has;
}

// --- Утилиты ---
static inline BYTE lerpB(BYTE a, BYTE b, float t) { return (BYTE)(a + (b - a) * t); }
static inline float clampf(float v, float a = 0.0f, float b = 1.0f) { return v < a ? a : (v > b ? b : v); }

static void PurgeTrayIcons(HWND owner) {
    // Удаляем все возможные варианты: по GUID, по hwndMain/Tray и uID=1
    NOTIFYICONDATA nd = {};
    nd.cbSize = sizeof(nd);
    nd.uID = 1;
    nd.uFlags = NIF_GUID;
    nd.guidItem = kTrayGuid;
    Shell_NotifyIcon(NIM_DELETE, &nd);
    nd.uFlags = 0;
    nd.hWnd = hwndMain; nd.uID = 1; Shell_NotifyIcon(NIM_DELETE, &nd);
    nd.hWnd = hwndTray; nd.uID = 1; Shell_NotifyIcon(NIM_DELETE, &nd);
}

// Полное удаление иконки из области уведомлений
static void DestroyTrayIcon() {
    PurgeTrayIcons(hwndMain);
    gTrayAdded = false;
}

static void EnsureTrayIconAdded(HWND owner) {
    (void)owner; // Трей отключён — не добавляем иконку
}

static void SetTrayIconVisible(bool visible) {
    (void)visible; // Трей отключён — управление видимостью не требуется
}

static wstring GetInstallConfigPath() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wstring p = exePath;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != wstring::npos) p = p.substr(0, pos + 1);
    else p += L"\\";
    return p + L"toggles.dat";
}

static wstring GetAppDataConfigPath() {
    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        wstring dir = wstring(appdata) + L"\\BlockWin";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\toggles.dat";
    }
    return GetInstallConfigPath();
}

static wstring GetAppDataGamesListPath() {
    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        wstring dir = wstring(appdata) + L"\\BlockWin";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\games.txt";
    }
    // рядом с exe
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wstring p = exePath; size_t pos = p.find_last_of(L"\\/");
    if (pos != wstring::npos) p = p.substr(0, pos + 1); else p += L"\\";
    return p + L"games.txt";
}

static void LoadGamesAllowlist() {
    gGameExeAllowlist.clear();
    wstring path = GetAppDataGamesListPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return; // нет файла — нет списка
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return; }
    vector<char> buf(size + 1, 0);
    DWORD read = 0; if (!ReadFile(h, buf.data(), size, &read, NULL)) { CloseHandle(h); return; }
    CloseHandle(h); buf[read] = 0;
    // Простейший разбор построчно (ASCII/ANSI достаточно для имён exe)
    string content(buf.data());
    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find_first_of("\r\n", start);
        string line = content.substr(start, (end == string::npos) ? string::npos : end - start);
        // trim
        size_t a = line.find_first_not_of(" \t");
        size_t b = line.find_last_not_of(" \t");
        if (a != string::npos) line = line.substr(a, b - a + 1); else line.clear();
        if (!line.empty() && line[0] != '#') {
            // к нижнему регистру и в wstring
            for (char& c : line) c = (char)tolower((unsigned char)c);
            wstring wline(line.begin(), line.end());
            gGameExeAllowlist.push_back(wline);
        }
        if (end == string::npos) break; else {
            if (end + 1 < content.size() && content[end] == '\r' && content[end + 1] == '\n') start = end + 2; else start = end + 1;
        }
    }
}

static bool IsExeInAllowlist(const wstring& exeLower) {
    for (const auto& it : gGameExeAllowlist) {
        if (it == exeLower) return true;
    }
    return false;
}

static void SaveTogglesToFile() {
    // Пытаемся сохранить в папку установки; если нет прав, используем AppData
    wstring path = GetInstallConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        path = GetAppDataConfigPath();
        h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0;
    const char header[4] = { 'T','G','0','1' };
    WriteFile(h, header, sizeof(header), &written, NULL);
    DWORD count = (DWORD)toggles.size();
    WriteFile(h, &count, sizeof(count), &written, NULL);
    for (const auto& t : toggles) {
        BYTE v = t->enabled.load() ? 1 : 0;
        WriteFile(h, &v, sizeof(v), &written, NULL);
    }
    CloseHandle(h);
}

static void LoadTogglesFromFile() {
    // Сначала пробуем рядом с exe, затем в AppData
    wstring path = GetInstallConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        path = GetAppDataConfigPath();
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD read = 0;
    char header[4] = {};
    if (!ReadFile(h, header, sizeof(header), &read, NULL) || read != sizeof(header)) { CloseHandle(h); return; }
    if (memcmp(header, "TG01", 4) != 0) { CloseHandle(h); return; }
    DWORD count = 0;
    if (!ReadFile(h, &count, sizeof(count), &read, NULL) || read != sizeof(count)) { CloseHandle(h); return; }
    DWORD n = min<DWORD>(count, (DWORD)toggles.size());
    for (DWORD i = 0; i < n; ++i) {
        BYTE v = 0;
        if (!ReadFile(h, &v, sizeof(v), &read, NULL) || read != sizeof(v)) break;
        toggles[i]->enabled.store(v != 0);
        toggles[i]->animLevel = v ? 1.0f : 0.0f;
    }
    CloseHandle(h);
}

void CenterWindowToScreen(HWND hwnd) {
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x = (sx - w) / 2;
    int y = (sy - h) / 2;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// ------------------- Проверка игры -------------------
bool IsGameRunningHeuristic() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hwndMain) return false;

    // Exclude desktop/shell and non-gamey windows
    if (!IsWindowVisible(fg) || IsIconic(fg)) return false;

    HWND root = GetAncestor(fg, GA_ROOT);
    if (root != fg) return false;

    wchar_t cls[256] = {0};
    GetClassName(fg, cls, 256);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0)
        return false;

    // Exclude common shell/UWP overlays
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
        wcscmp(cls, L"ApplicationFrameWindow") == 0 ||
        wcscmp(cls, L"Xaml_WindowedPopupClass") == 0)
        return false;

    if (GetWindow(fg, GW_OWNER) != NULL) return false;

    RECT r;
    if (!GetWindowRect(fg, &r)) return false;

    HMONITOR hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(hMon, &mi)) return false;

    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Более строгий порог полноэкранности, чтобы уменьшить ложные срабатывания
    bool nearlyFull = (w >= (int)(monW * 0.98f) && h >= (int)(monH * 0.98f));
    LONG_PTR style = GetWindowLongPtr(fg, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(fg, GWL_EXSTYLE);
    if ((style & WS_CHILD) || (exStyle & WS_EX_TOOLWINDOW)) return false;
    bool borderless = (style & WS_POPUP) && !(style & WS_CAPTION);
    bool maximized = IsZoomed(fg) != 0;

    // Проверяем foreground процесс: исключаем системные/неигровые, allowlist имеет приоритет
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!processHandle) return false;
    wchar_t pathBuf[MAX_PATH] = {0}; DWORD size = (DWORD)std::size(pathBuf);
    if (!QueryFullProcessImageNameW(processHandle, 0, pathBuf, &size)) { CloseHandle(processHandle); return false; }
    CloseHandle(processHandle);
    const wchar_t* fileName = wcsrchr(pathBuf, L'\\'); fileName = fileName ? fileName + 1 : pathBuf;
    wstring lowerName = fileName; for (wchar_t& ch : lowerName) ch = (wchar_t)towlower(ch);
    // Системные процессы — нет
    if (lowerName == L"explorer.exe" ||
        lowerName == L"shellexperiencehost.exe" ||
        lowerName == L"startmenuexperiencehost.exe" ||
        lowerName == L"searchhost.exe" ||
        lowerName == L"searchui.exe" ||
        lowerName == L"textinputhost.exe" ||
        lowerName == L"applicationframehost.exe") {
        return false;
    }
    // Явные не-игры — нет
    if (IsNonGameCommonProcess(lowerName)) return false;
    // Если явно в списке игр — пропускаем дальше на проверку полноэкранности
    // Иначе тоже позволяем, опираясь на полноэкранность, чтобы не ломать игры
    // (allowlist просто ускоряет/гарантирует срабатывание для специфичных игр)

    // Игра считается активной только если окно почти на весь монитор
    // и при этом оно или безрамочное (borderless), или развернуто на весь экран
    return nearlyFull && (borderless || maximized);
}

// ------------------- Хук -------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        // Блокировка только когда детектор явно подтвердил "Игровой режим"
        if (gameModeActive.load()) {
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if (toggles.size() > 0 && toggles[0]->enabled.load() && (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN))
                return 1;
            if (toggles.size() > 1 && toggles[1]->enabled.load() && alt && p->vkCode == VK_TAB) return 1;
            if (toggles.size() > 2 && toggles[2]->enabled.load() && alt && p->vkCode == VK_ESCAPE) return 1;
            if (toggles.size() > 3 && toggles[3]->enabled.load() && ctrl && (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN)) return 1;
            if (toggles.size() > 4 && toggles[4]->enabled.load() && shift && (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN)) return 1;
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

// ------------------- Neon Drawing -------------------
void DrawNeonRect(Graphics& g, int x, int y, int w, int h, Color baseColor) {
    Pen pen(baseColor, 2.0f);
    pen.SetLineJoin(LineJoinRound);

    float pulse = (sinf(neonPulse) + 1.0f) / 2.0f;
    int glowAdd = (int)(pulse * 100);

    for (int i = 0; i < 4; ++i) {
        pen.SetColor(Color(
            baseColor.GetA(),
            min(255, baseColor.GetR() + glowAdd),
            min(255, baseColor.GetG() + glowAdd),
            min(255, baseColor.GetB() + glowAdd)
        ));
        g.DrawRectangle(&pen, x - i, y - i, w + i * 2, h + i * 2);
    }
}

// ------------------- GUI -------------------
void DrawGUI(HDC hdc, int winW, int winH) {
    Bitmap buffer(winW, winH, PixelFormat32bppPARGB);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighQuality);

    // Тот же единый цвет, что и в кисти класса
    SolidBrush bgBrush(Color(255, kBgR, kBgG, kBgB));
    g.FillRectangle(&bgBrush, 0, 0, winW, winH);

    const int itemH = 56;
    const int gap = 12;
    const int gameBtnW = min(winW - 100, 600); // чуть увеличена ширина
    const int gameBtnH = itemH;
    const int togglesCount = (int)toggles.size();
    const int totalHeight = gameBtnH + gap + togglesCount * itemH + max(0, togglesCount - 1) * gap + 40;
    const int startY = (winH - totalHeight) / 2;
    const int centerX = winW / 2;
    const int gameBtnX = centerX - gameBtnW / 2;
    const int gameBtnY = startY;

    // Game button neon
    Color gBase(255, 0, 120, 255);
    DrawNeonRect(g, gameBtnX, gameBtnY, gameBtnW, gameBtnH, gBase);

    FontFamily ff(L"Segoe UI");
    Font font(&ff, 14, FontStyleRegular, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);

    SolidBrush txtBrush(Color(235, 235, 240));
    wstring gameText = gameModeActive.load() ? L"Игровой режим: ВКЛ" : L"Игровой режим: ВЫКЛ";
    g.DrawString(gameText.c_str(), -1, &font, RectF((REAL)gameBtnX, (REAL)gameBtnY, (REAL)gameBtnW, (REAL)gameBtnH), &fmt, &txtBrush);

    // Toggle buttons
    int toggleStartY = gameBtnY + gameBtnH + gap;
    for (int i = 0; i < togglesCount; ++i) {
        Toggle& t = *toggles[i];
        int x = gameBtnX;
        int y = toggleStartY + i * (itemH + gap);
        int w = gameBtnW;
        int h = itemH;

        Color baseColor(255, 0, 180, 255);
        DrawNeonRect(g, x, y, w, h, baseColor);

        Color textColor = Color(235, 235, 240);
        SolidBrush ttxt(textColor);
        g.DrawString(t.label.c_str(), -1, &font, RectF((REAL)x + 12.0f, (REAL)y, (REAL)w - 24.0f, (REAL)h), &fmt, &ttxt);

        int dotR = 10;
        int dotX = x + w - 20 - dotR;
        int dotY = y + (h - dotR * 2) / 2;
        Color dotColor = t.enabled.load() ? Color(255, 80, 200, 120) : Color(255, 120, 120, 120);
        SolidBrush dotBrush(dotColor);
        g.FillEllipse(&dotBrush, Rect(dotX, dotY, dotR * 2, dotR * 2));
        DrawNeonRect(g, dotX, dotY, dotR * 2, dotR * 2, Color(0, 0, 255, 255));
    }

    // Информационный текст снизу
    Font infoFont(&ff, 12, FontStyleRegular, UnitPixel);
    SolidBrush infoBrush(Color(150, 150, 255));
    wstring infoText = L"Чтобы блокировка заработала, войдите в игру!";
    g.DrawString(infoText.c_str(), -1, &infoFont, RectF(0, toggleStartY + togglesCount * (itemH + gap), (REAL)winW, 30), &fmt, &infoBrush);

    Graphics gMain(hdc);
    gMain.DrawImage(&buffer, 0, 0, winW, winH);
}

// ------------------- Анимация -------------------
void UpdateAnimationAndDetect() {
    bool detected = IsGameRunningHeuristic();
    gameModeActive.store(detected);

    // Динамически включаем/выключаем низкоуровневый хук только в режиме игры
    if (detected) {
        if (!hHook) {
            HMODULE mod = GetModuleHandle(NULL);
            hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, mod, 0);
        }
    } else {
        if (hHook) { UnhookWindowsHookEx(hHook); hHook = NULL; }
    }

    float targetGame = detected ? 1.0f : 0.0f;
    gameAnimLevel += (targetGame - gameAnimLevel) * 0.14f;
    gameAnimLevel = clampf(gameAnimLevel, 0.0f, 1.0f);

    for (auto& t : toggles) {
        float tgt = t->enabled.load() ? 1.0f : 0.0f;
        t->animLevel += (tgt - t->animLevel) * 0.14f;
        if (fabs(t->animLevel - tgt) < 0.005f) t->animLevel = tgt;
    }

    neonPulse += pulseSpeed;
    if (neonPulse > 3.14159265f * 2.0f) neonPulse -= 3.14159265f * 2.0f;

    if (hwndMain) InvalidateRect(hwndMain, NULL, FALSE);
}

// ------------------- WinProc -------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CenterWindowToScreen(hwnd);
        // Создаём контекстное меню трея
        gTrayMenu = CreatePopupMenu();
        if (gTrayMenu) {
            AppendMenuW(gTrayMenu, MF_STRING, ID_TRAY_OPEN, L"Открыть");
            AppendMenuW(gTrayMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");
        }
        break;
    case WM_ERASEBKGND:
        // Блокируем системную заливку, фон рисуем сами. Используем TRUE double-buffer при восстановлении.
        return 1;
    case WM_TIMER:
        if (wParam == 1) UpdateAnimationAndDetect();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        DrawGUI(hdc, rc.right - rc.left, rc.bottom - rc.top);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SHOWWINDOW:
        if (wParam) {
            // При показе окна очищаем возможные "хвосты" иконки трея
            DestroyTrayIcon();
        } else {
            // Трей отключён — ничего не делаем при скрытии окна
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rc; GetClientRect(hwnd, &rc);
        int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

        const int itemH = 56, gap = 12;
        const int gameBtnW = min(winW - 100, 600), gameBtnH = itemH;
        const int togglesCount = (int)toggles.size();
        const int totalHeight = gameBtnH + gap + togglesCount * itemH + max(0, togglesCount - 1) * gap + 40;
        const int startY = (winH - totalHeight) / 2;
        const int centerX = winW / 2;
        const int gameBtnX = centerX - gameBtnW / 2;
        const int gameBtnY = startY;

        RECT gameR = { gameBtnX, gameBtnY, gameBtnX + gameBtnW, gameBtnY + gameBtnH };
        if (PtInRect(&gameR, pt)) {
            if (IsGameRunningHeuristic())
                gameModeActive.store(!gameModeActive.load());
            return 0;
        }

        int toggleStartY = gameBtnY + gameBtnH + gap;
        for (int i = 0; i < togglesCount; ++i) {
            int x = gameBtnX, y = toggleStartY + i * (itemH + gap);
            RECT r = { x, y, x + gameBtnW, y + itemH };
            if (PtInRect(&r, pt)) {
                bool old = toggles[i]->enabled.load();
                toggles[i]->enabled.store(!old);
                SaveTogglesToFile();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        break;
    }
    case WM_CLOSE:
        // Больше не используем трей: по крестику просто сворачиваем в панель задач
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
    case WM_SYSCOMMAND:
        // Оставляем стандартное сворачивание в панель задач
        break;
    case WM_USER + 1: {
        // Трей отключён
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN: {
            WINDOWPLACEMENT wp = { sizeof(wp) };
            GetWindowPlacement(hwnd, &wp);
            EnableDarkTitleBar(hwnd, true);
            ShowWindow(hwnd, wp.showCmd == SW_SHOWMINIMIZED ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        }
        case ID_TRAY_EXIT:
            SaveTogglesToFile();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
        EnableDarkTitleBar(hwnd, true);
        if (gBgBrush) { DeleteObject(gBgBrush); gBgBrush = NULL; }
        gBgBrush = CreateSolidBrush(RGB(kBgR, kBgG, kBgB));
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_DESTROY:
        if (hHook) { UnhookWindowsHookEx(hHook); hHook = NULL; }
        DestroyTrayIcon();
        KillTimer(hwnd, 1);
        SaveTogglesToFile();
        GdiplusShutdown(gdiplusToken);
        if (gTrayMenu) { DestroyMenu(gTrayMenu); gTrayMenu = NULL; }
        if (gBgBrush) { DeleteObject(gBgBrush); gBgBrush = NULL; }
        if (gIconTray) { DestroyIcon(gIconTray); gIconTray = NULL; }
        if (gIconSmall) { DestroyIcon(gIconSmall); gIconSmall = NULL; }
        if (gIconBig) { DestroyIcon(gIconBig); gIconBig = NULL; }
        if (gSingleInstanceMutex) { CloseHandle(gSingleInstanceMutex); gSingleInstanceMutex = NULL; }
        if (hwndTray) { DestroyWindow(hwndTray); hwndTray = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        // Трей отключён
        if (msg == gMsgTaskbarCreated) { return 0; }
        if (msg == gMsgActivate) {
            // Перед активацией главного окна — удалим иконку, чтобы исключить дубли
            DestroyTrayIcon();
            WINDOWPLACEMENT wp = { sizeof(wp) };
            GetWindowPlacement(hwnd, &wp);
            EnableDarkTitleBar(hwnd, true);
            ShowWindow(hwnd, wp.showCmd == SW_SHOWMINIMIZED ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ------------------- WinMain -------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    hInst = hInstance;

    // Single-instance mutex (локальный сеанс, чтобы не требовать UAC разных контекстов)
    gSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\BlockWin_SingleInstance_Mutex");
    // Обработка параметра --activate: только активируем и выходим
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool onlyActivate = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--activate") == 0) { onlyActivate = true; break; }
    }
    if (argv) LocalFree(argv);

    if (onlyActivate) {
        HWND existing = FindWindowW(L"BlockWinApp", L"BlockWin — Киберпанк режим");
        if (existing) {
            PostMessageW(existing, gMsgActivate, 0, 0);
            return 0;
        }
        // Если не найдено — продолжим обычный запуск ниже
    } else if (gSingleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"BlockWinApp", L"BlockWin — Киберпанк режим");
        if (existing) PostMessageW(existing, gMsgActivate, 0, 0);
        else PostMessageW(HWND_BROADCAST, gMsgActivate, 0, 0);
        return 0;
    }

    // Не задаём явный AppUserModelID — пусть группировка идёт по пути exe

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    toggles.clear();
    toggles.emplace_back(make_unique<Toggle>(L"Блокировать Win", true));
    toggles.emplace_back(make_unique<Toggle>(L"Блокировать Alt+Tab", true));
    toggles.emplace_back(make_unique<Toggle>(L"Блокировать Alt+Esc", false));
    toggles.emplace_back(make_unique<Toggle>(L"Блокировать Ctrl+Win", false));
    toggles.emplace_back(make_unique<Toggle>(L"Блокировать Shift+Win", false));
    LoadTogglesFromFile();
    LoadGamesAllowlist();

    const wchar_t* kClassName = L"BlockWinApp";
    const wchar_t* kIconPath = L"D:\\script\\BlockWin\\BlockWin\\Gemini_Generated_Image_kgu8lgkgu8lgkgu8.ico";
    gBgBrush = CreateSolidBrush(RGB(kBgR, kBgG, kBgB));
    gIconBig = (HICON)LoadImageW(NULL, kIconPath, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    gIconSmall = (HICON)LoadImageW(NULL, kIconPath, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    gMsgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    gMsgActivate = RegisterWindowMessageW(L"BlockWin_Activate");
    WNDCLASSEX wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = 0; // избегаем лишних перерисовок, которые дают мерцание
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = kClassName;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = gBgBrush;
    wcex.hIcon = gIconBig ? gIconBig : (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_BLOCKWIN));
    wcex.hIconSm = gIconSmall ? gIconSmall : (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    RegisterClassEx(&wcex);

    // Регистрируем скрытый класс окна для трея
    WNDCLASSEX wct = {};
    wct.cbSize = sizeof(WNDCLASSEX);
    wct.style = 0;
    wct.lpfnWndProc = TrayWndProc;
    wct.hInstance = hInstance;
    wct.lpszClassName = L"BlockWinTrayHost";
    RegisterClassEx(&wct);

    int W = 700, H = 520; // увеличенный размер окна
    hwndMain = CreateWindowEx(0, kClassName, L"BlockWin — Киберпанк режим",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hInstance, NULL);

    // Трей отключён: не создаём message-only окно
    hwndTray = NULL;

    // Гарантированно удаляем любые хвосты иконки трея до первого показа
    DestroyTrayIcon();

    // Настраиваем стили до первого показа
    EnableDarkTitleBar(hwndMain, true);
    SetAppUserModelForWindow(hwndMain);
    LONG_PTR ex = GetWindowLongPtr(hwndMain, GWL_EXSTYLE);
    ex |= WS_EX_APPWINDOW;
    ex &= ~WS_EX_TOOLWINDOW;
    SetWindowLongPtr(hwndMain, GWL_EXSTYLE, ex);
    ShowWindow(hwndMain, nCmdShow);
    // Синхронизируем AUMID закреплённых ярлыков, чтобы окно группировалось корректно
    CoInitialize(NULL);
    AlignPinnedTaskbarShortcutsAUMID();
    CoUninitialize();
    if (gIconBig) SendMessage(hwndMain, WM_SETICON, ICON_BIG, (LPARAM)gIconBig);
    if (gIconSmall) SendMessage(hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)gIconSmall);

    // Трей-иконка отключена
    gIconTray = NULL;

    SetTimer(hwndMain, 1, 16, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
