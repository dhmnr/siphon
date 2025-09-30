#include "process_memory.h"
#include "process_attribute.h"
#include "utils.h"
#include <algorithm>
#include <iostream>
#include <ostream>
#include <psapi.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <tlhelp32.h>
#include <vector>
#include <windows.h>

ProcessMemory::ProcessMemory(const std::string &processName,
                             const std::map<std::string, ProcessAttribute> &processAttributes)
    : processId(0), processHandle(nullptr), baseAddress(0), moduleSize(0), processName(processName),
      processAttributes(processAttributes) {}

ProcessMemory::~ProcessMemory() {
    if (processHandle) {
        CloseHandle(processHandle);
    }
}

DWORD ProcessMemory::FindProcessByName(const std::string &processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32 processEntry;
    processEntry.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(snapshot, &processEntry)) {
        do {
            if (processName == processEntry.szExeFile) {
                CloseHandle(snapshot);
                return processEntry.th32ProcessID;
            }
        } while (Process32Next(snapshot, &processEntry));
    }

    CloseHandle(snapshot);
    return 0;
}

bool ProcessMemory::GetModuleInfo() {
    HMODULE modules[1024];
    DWORD bytesNeeded;

    if (EnumProcessModules(processHandle, modules, sizeof(modules), &bytesNeeded)) {
        MODULEINFO moduleInfo;
        if (GetModuleInformation(processHandle, modules[0], &moduleInfo, sizeof(moduleInfo))) {
            baseAddress = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
            moduleSize = moduleInfo.SizeOfImage;
            return true;
        }
    }
    return false;
}

bool ProcessMemory::Initialize() {
    if (!IsRunAsAdmin()) {
        spdlog::error("ERROR: Must run as Administrator!");
        return false;
    }

    processId = FindProcessByName(processName);
    if (processId == 0) {
        spdlog::error("{} not found!", processName);
        return false;
    }

    processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!processHandle) {
        spdlog::error("Failed to open process. Error: {}", GetLastError());
        return false;
    }

    if (!GetModuleInfo()) {
        spdlog::error("Failed to get module information!");
        return false;
    }

    spdlog::info("Successfully attached to {} (PID: {})", processName, processId);
    spdlog::info("Base address: 0x{:x}", baseAddress);
    spdlog::info("Module size: 0x{:x}", moduleSize);
    spdlog::info("Base address: 0x{:x}", baseAddress);
    spdlog::info("Module size: 0x{:x}", moduleSize);

    return true;
}

std::vector<uint8_t> ProcessMemory::ParseAOB(const std::string &pattern) {
    std::vector<uint8_t> bytes;
    std::istringstream iss(pattern);
    std::string token;

    while (iss >> token) {
        if (token == "??") {
            bytes.push_back(0x00);
        } else {
            try {
                bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
            } catch (...) {
                spdlog::error("Invalid hex token: {}", token);
                return {};
            }
        }
    }
    return bytes;
}

std::vector<bool> ProcessMemory::ParseWildcards(const std::string &pattern) {
    std::vector<bool> wildcards;
    std::istringstream iss(pattern);
    std::string token;

    while (iss >> token) {
        wildcards.push_back(token == "??");
    }
    return wildcards;
}

uintptr_t ProcessMemory::AOBScan(const std::string &pattern, std::vector<bool> wildcards) {
    std::vector<uint8_t> patternBytes = ParseAOB(pattern);

    if (patternBytes.empty()) {
        spdlog::error("Invalid pattern!");
        return 0;
    }

    spdlog::info("Scanning for pattern: {}", pattern);
    spdlog::info("Pattern length: {} bytes", patternBytes.size());

    const size_t chunkSize = 0x10000; // 64KB chunks
    std::vector<uint8_t> buffer(chunkSize);

    for (size_t offset = 0; offset < moduleSize; offset += chunkSize) {
        size_t readSize = min(chunkSize, moduleSize - offset);
        uintptr_t currentAddress = baseAddress + offset;

        SIZE_T bytesRead;
        if (!ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(currentAddress),
                               buffer.data(), readSize, &bytesRead)) {
            continue; // Skip unreadable regions
        }

        for (size_t i = 0; i <= bytesRead - patternBytes.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < patternBytes.size(); ++j) {
                if (!wildcards[j] && buffer[i + j] != patternBytes[j]) {
                    found = false;
                    break;
                }
            }

            if (found) {
                uintptr_t foundAddress = currentAddress + i;
                spdlog::info("Pattern found at: 0x{:x}", foundAddress);
                return foundAddress;
            }
        }

        // Progress indicator
        if (offset % 0x100000 == 0) { // Every 1MB
            spdlog::info("Scanned: {}%", (offset * 100) / moduleSize);
        }
    }

    spdlog::error("Pattern not found!");
    return 0;
}

uintptr_t ProcessMemory::ExtractPtrFromInst(uintptr_t instructionAddress, int addressStartIndex) {
    uint8_t instruction[16];
    SIZE_T bytesRead;

    if (!ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(instructionAddress),
                           instruction, sizeof(instruction), &bytesRead)) {
        spdlog::error("Failed to read instruction at 0x{:x}", instructionAddress);
        return 0;
    }

    int32_t offset = *reinterpret_cast<int32_t *>(&instruction[addressStartIndex]);
    uintptr_t targetAddress = instructionAddress + 7 + offset; // 7 = instruction length

    spdlog::info("Found mov instruction with RIP-relative addressing");
    spdlog::info("Target address: 0x{:x}", targetAddress);

    return targetAddress;
    // TODO : Error handling
}

uintptr_t ProcessMemory::FindPtrFromAOB(const std::string &pattern) {

    std::vector<bool> wildcards = ParseWildcards(pattern);
    uintptr_t instructionAddress = AOBScan(pattern, wildcards);
    if (instructionAddress == 0) {
        return 0;
    }
    int addressStartIndex =
        static_cast<int>(std::find(wildcards.begin(), wildcards.end(), true) - wildcards.begin());
    uintptr_t pointerAddress = ExtractPtrFromInst(instructionAddress, addressStartIndex);
    if (pointerAddress == 0) {
        return 0;
    }

    // Read the actual pointer value
    uintptr_t ptrAddress;
    SIZE_T bytesRead;
    if (!ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(pointerAddress), &ptrAddress,
                           sizeof(ptrAddress), &bytesRead)) {
        spdlog::error("Failed to read pointer at 0x{:x}", pointerAddress);
        return 0;
    }

    spdlog::info("Pointer found at: 0x{:x}", ptrAddress);
    return ptrAddress;
}

bool ProcessMemory::ReadPtr(uintptr_t address, uintptr_t &value) {
    SIZE_T bytesRead;
    return ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(uintptr_t), &bytesRead) &&
           bytesRead == sizeof(uintptr_t);
}

bool ProcessMemory::ReadInt32(uintptr_t address, int32_t &value) {
    SIZE_T bytesRead;
    return ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(int32_t), &bytesRead) &&
           bytesRead == sizeof(int32_t);
}

bool ProcessMemory::WriteInt32(uintptr_t address, const int32_t &value) {
    SIZE_T bytesWritten;
    return WriteProcessMemory(processHandle, reinterpret_cast<LPVOID>(address), &value,
                              sizeof(int32_t), &bytesWritten) &&
           bytesWritten == sizeof(int32_t);
}

uintptr_t ProcessMemory::ResolvePointerChain(uintptr_t baseAddress,
                                             const std::vector<uintptr_t> &offsets) {
    uintptr_t currentAddress = baseAddress;
    // TODO : Error handling
    spdlog::info("Starting pointer chain resolution:");
    spdlog::info("Base: 0x{:x}", currentAddress);

    for (size_t i = 0; i < offsets.size() - 1; ++i) {
        if (currentAddress == 0) {
            spdlog::error("Null pointer encountered at level {}", i);
            return 0;
        }

        // First add the offset to current address
        uintptr_t addressToRead = currentAddress + offsets[i];
        spdlog::info("0x{:x} + 0x{:x} = 0x{:x}", currentAddress, offsets[i], addressToRead);

        // Then read the pointer at that address
        uintptr_t nextAddress;
        if (!ReadPtr(addressToRead, nextAddress)) {
            spdlog::error(" -> Failed to read pointer");
            return 0;
        }

        spdlog::info(" -> 0x{:x}", nextAddress);
        currentAddress = nextAddress;
    }

    uintptr_t finalAddress = currentAddress + offsets[offsets.size() - 1];
    spdlog::info("0x{:x} + 0x{:x} = 0x{:x}", currentAddress, offsets[offsets.size() - 1],
                 finalAddress);

    spdlog::info("Final address: 0x{:x}", finalAddress);
    return finalAddress;
}

bool ProcessMemory::ExtractAttribute(std::string attributeName, int32_t &value) {
    uintptr_t ptr = FindPtrFromAOB(processAttributes[attributeName].AttributePattern);
    spdlog::info("Pointer found at: 0x{:x}", ptr);

    uintptr_t attributeAddress =
        ResolvePointerChain(ptr, processAttributes[attributeName].AttributeOffsets);
    spdlog::info("{} found at: 0x{:x}", attributeName, attributeAddress);

    if (!ReadInt32(attributeAddress, value)) {
        return false;
    }
    spdlog::info("{} value: {}", attributeName, value);

    return true;
}

bool ProcessMemory::WriteAttribute(std::string attributeName, const int32_t &value) {
    uintptr_t ptr = FindPtrFromAOB(processAttributes[attributeName].AttributePattern);

    spdlog::info("Pointer found at: 0x{:x}", ptr);

    uintptr_t attributeAddress =
        ResolvePointerChain(ptr, processAttributes[attributeName].AttributeOffsets);
    spdlog::info("{} found at: 0x{:x}", attributeName, attributeAddress);

    return WriteInt32(attributeAddress, value);
}