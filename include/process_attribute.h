#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ProcessAttribute {
  public:
    std::string AttributeName;
    std::string AttributePattern;
    std::vector<uintptr_t> AttributeOffsets;
    std::string AttributeType;
    size_t AttributeLength;
    std::string AttributeMethod;
};

// const std::string SiphonAttributes::WorldChrMan = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88";
// const std::vector<uintptr_t> SiphonAttributes::HpOffsets = {0x10EF8, 0x0, 0x190, 0x0, 0x138};

bool GetProcessInfoFromTOML(const std::string &filepath, std::string *processName,
                            std::map<std::string, ProcessAttribute> *attributes,
                            std::string *processWindowName);
void PrintProcessAttributes(const std::map<std::string, ProcessAttribute> &attributes);