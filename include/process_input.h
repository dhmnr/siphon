#pragma once

#include "interception.h"
#include <cstdio>
#include <map>
#include <string>
#include <windows.h>

extern std::map<std::string, unsigned short> scancodeMap;

class ProcessInput {
  private:
    HWND gameWindow;
    InterceptionContext context;
    InterceptionDevice keyboard;
    std::string processWindowName;

    struct EnumWindowsData {
        const std::string *processWindowName;
        HWND *gameWindow;
    };

    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
        char title[256];
        char className[256];

        GetWindowTextA(hwnd, title, sizeof(title));
        GetClassNameA(hwnd, className, sizeof(className));

        EnumWindowsData *data = reinterpret_cast<EnumWindowsData *>(lParam);

        // Debug: Print all windows that contain our target name
        if (strstr(title, data->processWindowName->c_str()) != nullptr ||
            strstr(className, data->processWindowName->c_str()) != nullptr) {
            printf("DEBUG: Found window - Title: '%s', Class: '%s', Visible: %s\n", title,
                   className, IsWindowVisible(hwnd) ? "Yes" : "No");
        }

        // Check if this is the process (starts with "processWindowName")
        size_t nameLen = data->processWindowName->length();
        if (strncmp(title, data->processWindowName->c_str(), nameLen) == 0 ||
            strncmp(className, data->processWindowName->c_str(), nameLen) == 0) {
            if (IsWindowVisible(hwnd)) {
                *(data->gameWindow) = hwnd;
                printf("DEBUG: Selected window - Title: '%s', HWND: 0x%p\n", title, hwnd);
                return FALSE; // Stop enumeration
            }
        }
        return TRUE; // Continue enumeration
    }

  public:
    ProcessInput(const std::string &processWindowName);
    ~ProcessInput();
    bool IsInitialized() const;
    bool BringToFocus();
    void PressKey(std::string key);
    void ReleaseKey(std::string key);
    void TapKey(std::string key, int holdMs = 5000);
};