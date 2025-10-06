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

bool GetProcessInfoFromTOML(const std::string &filepath, std::string *processName,
                            std::map<std::string, ProcessAttribute> *processAttributes,
                            std::string *processWindowName) {
    if (!processName || !processAttributes) {
        return false;
    }

    processAttributes->clear();

    try {
        toml::table config = toml::parse_file(filepath);

        // Extract process name
        if (auto process_info = config["process_info"].as_table()) {
            if (auto name = (*process_info)["name"].value<std::string>()) {
                *processName = *name;
            }
            if (auto window_name = (*process_info)["window_name"].value<std::string>()) {
                *processWindowName = *window_name;
            }
        }

        // Extract attributes
        if (auto attrs = config["attributes"].as_table()) {
            for (auto &&[key, value] : *attrs) {
                if (auto attr_table = value.as_table()) {
                    ProcessAttribute attr;
                    attr.AttributeName = std::string(key.str());

                    if (auto pattern = (*attr_table)["pattern"].value<std::string>()) {
                        attr.AttributePattern = *pattern;
                    }

                    if (auto type = (*attr_table)["type"].value<std::string>()) {
                        attr.AttributeType = *type;
                    }

                    if (auto offsets = (*attr_table)["offsets"].as_array()) {
                        for (auto &&offset : *offsets) {
                            if (auto offset_val = offset.value<int64_t>()) {
                                attr.AttributeOffsets.push_back(
                                    static_cast<uintptr_t>(*offset_val));
                            }
                        }
                    }

                    if (auto length = (*attr_table)["length"].value<size_t>()) {
                        attr.AttributeLength = *length;
                    }

                    // Insert into map with key as the attribute name
                    (*processAttributes)[attr.AttributeName] = attr;
                }
            }
        }

        return true;
    } catch (const toml::parse_error &err) {
        std::cerr << "TOML parsing error: " << err << std::endl;
        return false;
    }
}

void PrintProcessAttributes(const std::map<std::string, ProcessAttribute> &attributes) {
    for (const auto &[name, attr] : attributes) {
        std::string offsets;
        for (const auto &offset : attr.AttributeOffsets) {
            std::stringstream ss;
            ss << "0x" << std::hex << offset;
            offsets += ss.str() + " ";
        }
        if (attr.AttributeType == "array") {
            spdlog::info("Attribute: {} | Type: {} | Pattern: {} | Offsets: {} | Length: {}", name,
                         attr.AttributeType, attr.AttributePattern, offsets, attr.AttributeLength);
        } else {
            spdlog::info("Attribute: {} | Type: {} | Pattern: {} | Offsets: {}", name,
                         attr.AttributeType, attr.AttributePattern, offsets);
        }
    }
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
