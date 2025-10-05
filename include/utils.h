#pragma once

#include "process_attribute.h"
#include <map>
#include <string>
#include <windows.h>

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
    // if (strstr(title, data->processWindowName->c_str()) != nullptr ||
    //     strstr(className, data->processWindowName->c_str()) != nullptr) {
    //     // printf("DEBUG: Found window - Title: '%s', Class: '%s', Visible: %s\n", title,
    //     className,
    //         //    IsWindowVisible(hwnd) ? "Yes" : "No");
    // }

    // Check if this is the process (starts with "processWindowName")
    size_t nameLen = data->processWindowName->length();
    if (strncmp(title, data->processWindowName->c_str(), nameLen) == 0 ||
        strncmp(className, data->processWindowName->c_str(), nameLen) == 0) {
        if (IsWindowVisible(hwnd)) {
            *(data->gameWindow) = hwnd;
            // printf("DEBUG: Selected window - Title: '%s', HWND: 0x%p\n", title, hwnd);
            return FALSE; // Stop enumeration
        }
    }
    return TRUE; // Continue enumeration
}

bool IsRunAsAdmin();
bool GetProcessInfoFromTOML(const std::string &filepath, std::string *processName,
                            std::map<std::string, ProcessAttribute> *attributes,
                            std::string *processWindowName);
void PrintProcessAttributes(const std::map<std::string, ProcessAttribute> &attributes);
bool GetProcessWindow(const std::string *processWindowName, HWND *gameWindow);
bool BringToFocus(HWND processWindow);