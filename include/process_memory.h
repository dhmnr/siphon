#pragma once

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

  public:
    ProcessMemory(const std::string &procName);
    ~ProcessMemory();

    DWORD FindProcessByName(const std::string &processName);
    bool GetModuleInfo();
    bool Initialize();
    std::vector<uint8_t> ParseAOB(const std::string &pattern);
    std::vector<bool> ParseWildcards(const std::string &pattern);
    uintptr_t AOBScan(const std::string &pattern, std::vector<bool> wildcards);
    uintptr_t ExtractPtrFromInst(uintptr_t instructionAddress, int addressStartIndex);
    uintptr_t FindPtrFromAOB(const std::string &pattern);
    bool ReadPtr(uintptr_t address, uintptr_t &value);
    bool ReadInt32(uintptr_t address, int32_t &value);
    bool WriteInt32(uintptr_t address, const int32_t &value);
    uintptr_t ResolvePointerChain(uintptr_t baseAddress, const std::vector<uintptr_t> &offsets);
};