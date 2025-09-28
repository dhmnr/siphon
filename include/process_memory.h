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
    uintptr_t ExtractPtrFromInst(uintptr_t instructionAddress,
                                 int addressStartIndex);
    uintptr_t FindPtrFromAOB(const std::string &pattern);
};