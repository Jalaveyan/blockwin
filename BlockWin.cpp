#include <windows.h>
#include <iostream>
#include <vector>

HHOOK hHook = NULL;

bool IsGameWindow(HWND hwnd) {
    if (!hwnd) return false;

    RECT rect;
    GetWindowRect(hwnd, &rect);

    int tolerance = 5;

    std::vector<RECT> monitors;
    EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(hMon, &mi);
        std::vector<RECT>* mons = (std::vector<RECT>*)lParam;
        mons->push_back(mi.rcMonitor);
        return TRUE;
        }, (LPARAM)&monitors);

    for (auto& mon : monitors) {
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        int monW = mon.right - mon.left;
        int monH = mon.bottom - mon.top;

        if (rect.left == mon.left && rect.top == mon.top && width == monW && height == monH)
            return true;

        if (abs(width - monW) <= tolerance && abs(height - monH) <= tolerance)
            return true;
    }

    return false;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        HWND hwnd = GetForegroundWindow();
        bool isGame = IsGameWindow(hwnd);

        bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        if (isGame &&
            (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) &&
            !altPressed) {
            return 1; // блокируем
        }
    }

    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

int main() {
    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!hHook) {
        std::cout << "Hook failed!" << std::endl;
        return 1;
    }

    std::cout << "BlockWin running. Press Ctrl+C to exit." << std::endl;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hHook);
    return 0;
}
