#include "process_memory.h"
#include "utils.h"
#include <algorithm>
#include <iostream>
#include <ostream>
#include <psapi.h>
#include <sstream>
#include <tlhelp32.h>
#include <vector>
#include <windows.h>

ProcessMemory::ProcessMemory(const std::string &procName)
    : processId(0), processHandle(nullptr), baseAddress(0), moduleSize(0), processName(procName) {}

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
        std::cout << "ERROR: Must run as Administrator!" << std::endl;
        return false;
    }

    processId = FindProcessByName(processName);
    if (processId == 0) {
        std::cout << processName << " not found!" << std::endl;
        return false;
    }

    processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (!processHandle) {
        std::cout << "Failed to open process. Error: " << GetLastError() << std::endl;
        return false;
    }

    if (!GetModuleInfo()) {
        std::cout << "Failed to get module information!" << std::endl;
        return false;
    }

    std::cout << "Successfully attached to " << processName << " (PID: " << processId << ")"
              << std::endl;
    std::cout << "Base address: 0x" << std::hex << baseAddress << std::endl;
    std::cout << "Module size: 0x" << std::hex << moduleSize << std::endl;

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
                std::cout << "Invalid hex token: " << token << std::endl;
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
        std::cout << "Invalid pattern!" << std::endl;
        return 0;
    }

    std::cout << "Scanning for pattern: " << pattern << std::endl;
    std::cout << "Pattern length: " << patternBytes.size() << " bytes" << std::endl;

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
                std::cout << "Pattern found at: 0x" << std::hex << foundAddress << std::endl;
                return foundAddress;
            }
        }

        // Progress indicator
        if (offset % 0x100000 == 0) { // Every 1MB
            std::cout << "Scanned: " << std::dec << (offset * 100) / moduleSize << "%" << std::endl;
        }
    }

    std::cout << "Pattern not found!" << std::endl;
    return 0;
}

uintptr_t ProcessMemory::ExtractPtrFromInst(uintptr_t instructionAddress, int addressStartIndex) {
    uint8_t instruction[16];
    SIZE_T bytesRead;

    if (!ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(instructionAddress),
                           instruction, sizeof(instruction), &bytesRead)) {
        std::cout << "Failed to read instruction at 0x" << std::hex << instructionAddress
                  << std::endl;
        return 0;
    }

    int32_t offset = *reinterpret_cast<int32_t *>(&instruction[addressStartIndex]);
    uintptr_t targetAddress = instructionAddress + 7 + offset; // 7 = instruction length

    std::cout << "Found mov instruction with RIP-relative addressing" << std::endl;
    std::cout << "Target address: 0x" << std::hex << targetAddress << std::endl;

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
        std::cout << "Failed to read pointer at 0x" << std::hex << pointerAddress << std::endl;
        return 0;
    }

    std::cout << "Pointer found at: 0x" << std::hex << ptrAddress << std::endl;
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
    std::cout << "Starting pointer chain resolution:" << std::endl;
    std::cout << "Base: 0x" << std::hex << currentAddress << std::endl;

    for (size_t i = 0; i < offsets.size() - 1; ++i) {
        if (currentAddress == 0) {
            std::cout << "Null pointer encountered at level " << i << std::endl;
            return 0;
        }

        // First add the offset to current address
        uintptr_t addressToRead = currentAddress + offsets[i];
        std::cout << "0x" << std::hex << currentAddress << " + 0x" << offsets[i] << " = 0x"
                  << addressToRead;

        // Then read the pointer at that address
        uintptr_t nextAddress;
        if (!ReadPtr(addressToRead, nextAddress)) {
            std::cout << " -> Failed to read pointer" << std::endl;
            return 0;
        }

        std::cout << " -> 0x" << std::hex << nextAddress << std::endl;
        currentAddress = nextAddress;
    }

    uintptr_t finalAddress = currentAddress + offsets[offsets.size() - 1];
    std::cout << "0x" << std::hex << currentAddress << " + 0x" << offsets[offsets.size() - 1]
              << " = 0x" << finalAddress;

    std::cout << "Final address: 0x" << std::hex << finalAddress << std::endl;
    return finalAddress;
}