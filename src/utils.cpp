#include "process_attribute.h"
#include <iostream>
#include <spdlog/spdlog.h>
#include <toml++/toml.h>
#include <vector>
#include <windows.h>

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
                            std::map<std::string, ProcessAttribute> *processAttributes) {
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
        spdlog::info("Attribute Name: {}", name);
        spdlog::info("Attribute Pattern: {}", attr.AttributePattern);
        spdlog::info("Attribute Type: {}", attr.AttributeType);
        spdlog::info("Attribute Offsets: ");
        for (const auto &offset : attr.AttributeOffsets) {
            spdlog::info("0x{:x}", offset);
        }
        spdlog::info("Attribute Offsets: ");
    }
}