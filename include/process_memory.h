#pragma once

#include "process_attribute.h"
#include <map>
#include <string>
#include <vector>
#include <windows.h>

class ProcessMemory {
  private:
    DWORD processId;
    HANDLE processHandle;
    uintptr_t baseAddress;
    size_t moduleSize;
    std::string processName;
    std::map<std::string, ProcessAttribute> processAttributes;

  public:
    ProcessMemory(const std::string &processName,
                  const std::map<std::string, ProcessAttribute> &processAttributes);
    ~ProcessMemory();

    DWORD FindProcessByName(const std::string &processName);
    bool GetModuleInfo();
    bool Initialize();
    ProcessAttribute GetAttribute(std::string attributeName);
    std::vector<uint8_t> ParseAOB(const std::string &pattern);
    std::vector<bool> ParseWildcards(const std::string &pattern);
    uintptr_t AOBScan(const std::string &pattern, std::vector<bool> wildcards);
    uintptr_t ExtractPtrFromInst(uintptr_t instructionAddress, int addressStartIndex);
    uintptr_t FindPtrFromAOB(const std::string &pattern);
    uintptr_t ResolvePointerChain(uintptr_t baseAddress, const std::vector<uintptr_t> &offsets);
    bool ReadPtr(uintptr_t address, uintptr_t &value);
    bool ReadInt(uintptr_t address, int32_t &value);
    bool WriteInt(uintptr_t address, const int32_t &value);
    bool ReadFloat(uintptr_t address, float &value);
    bool WriteFloat(uintptr_t address, const float &value);
    bool ReadArray(uintptr_t address, std::vector<uint8_t> &value);
    bool WriteArray(uintptr_t address, const std::vector<uint8_t> &value);
    bool ExtractAttributeInt(std::string attributeName, int32_t &value);
    bool WriteAttributeInt(std::string attributeName, const int32_t &value);
    bool ExtractAttributeFloat(std::string attributeName, float &value);
    bool WriteAttributeFloat(std::string attributeName, const float &value);
    bool ExtractAttributeArray(std::string attributeName, std::vector<uint8_t> &value);
    bool WriteAttributeArray(std::string attributeName, const std::vector<uint8_t> &value);
};