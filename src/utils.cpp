#include "process_attribute.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <toml++/toml.h>
#include <vector>
#include <windows.h>

#include "utils.h"

bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    HANDLE token = nullptr;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);

        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            isAdmin = elevation.TokenIsElevated;
        }
    }

    if (token) {
        CloseHandle(token);
    }

    return isAdmin;
}

bool GetProcessWindow(const std::string *processWindowName, HWND *gameWindow) {

    EnumWindowsData data = {processWindowName, gameWindow};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return *gameWindow != nullptr;
}

// Ensure game window is in focus
bool BringToFocus(HWND processWindow) {
    if (!processWindow)
        return false;

    // Check if already focused
    if (GetForegroundWindow() == processWindow) {
        return true;
    }

    keybd_event(VK_MENU, 0, 0, 0); // Alt down
    SetForegroundWindow(processWindow);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0); // Alt up

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    return GetForegroundWindow() == processWindow;
}
