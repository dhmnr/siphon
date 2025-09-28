#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <tlhelp32.h>
#include <sstream>
#include <algorithm>

class EldenRingMemory {
private:
    DWORD processId;
    HANDLE processHandle;
    uintptr_t baseAddress;
    size_t moduleSize;

public:
    EldenRingMemory() : processId(0), processHandle(nullptr), baseAddress(0), moduleSize(0) {}
    
    ~EldenRingMemory() {
        if (processHandle) {
            CloseHandle(processHandle);
        }
    }

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

    DWORD FindProcessByName(const std::string& processName) {
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

    bool GetModuleInfo() {
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

    bool Initialize() {
        if (!IsRunAsAdmin()) {
            std::cout << "ERROR: Must run as Administrator!" << std::endl;
            return false;
        }
        
        processId = FindProcessByName("eldenring.exe");
        if (processId == 0) {
            std::cout << "eldenring.exe not found!" << std::endl;
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
        
        std::cout << "Successfully attached to eldenring.exe (PID: " << processId << ")" << std::endl;
        std::cout << "Base address: 0x" << std::hex << baseAddress << std::endl;
        std::cout << "Module size: 0x" << std::hex << moduleSize << std::endl;
        
        return true;
    }

    std::vector<uint8_t> ParseAOB(const std::string& pattern) {
        std::vector<uint8_t> bytes;
        std::istringstream iss(pattern);
        std::string token;
        
        while (iss >> token) {
            if (token == "??") {
                bytes.push_back(0x00); // We'll handle wildcards separately
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

    std::vector<bool> ParseWildcards(const std::string& pattern) {
        std::vector<bool> wildcards;
        std::istringstream iss(pattern);
        std::string token;
        
        while (iss >> token) {
            wildcards.push_back(token == "??");
        }
        return wildcards;
    }

    uintptr_t AOBScan(const std::string& pattern) {
        std::vector<uint8_t> patternBytes = ParseAOB(pattern);
        std::vector<bool> wildcards = ParseWildcards(pattern);
        
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
            if (!ReadProcessMemory(processHandle, 
                                 reinterpret_cast<LPCVOID>(currentAddress), 
                                 buffer.data(), 
                                 readSize, 
                                 &bytesRead)) {
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

    uintptr_t ExtractPointerFromInstruction(uintptr_t instructionAddress) {
        uint8_t instruction[16];
        SIZE_T bytesRead;
        
        if (!ReadProcessMemory(processHandle, 
                             reinterpret_cast<LPCVOID>(instructionAddress), 
                             instruction, 
                             sizeof(instruction), 
                             &bytesRead)) {
            std::cout << "Failed to read instruction at 0x" << std::hex << instructionAddress << std::endl;
            return 0;
        }

        // Check for common pointer instruction patterns
        // Pattern: 48 8B 05 xx xx xx xx (mov rax, [rip+offset])
        if (instruction[0] == 0x48 && instruction[1] == 0x8B && instruction[2] == 0x05) {
            int32_t offset = *reinterpret_cast<int32_t*>(&instruction[3]);
            uintptr_t targetAddress = instructionAddress + 7 + offset; // 7 = instruction length
            
            std::cout << "Found mov instruction with RIP-relative addressing" << std::endl;
            std::cout << "Instruction: 48 8B 05 " << std::hex << std::setfill('0') << std::setw(8) << offset << std::endl;
            std::cout << "Target address: 0x" << std::hex << targetAddress << std::endl;
            
            return targetAddress;
        }
        
        // Pattern: 48 8B 3D xx xx xx xx (mov rdi, [rip+offset])
        if (instruction[0] == 0x48 && instruction[1] == 0x8B && instruction[2] == 0x3D) {
            int32_t offset = *reinterpret_cast<int32_t*>(&instruction[3]);
            uintptr_t targetAddress = instructionAddress + 7 + offset;
            
            std::cout << "Found mov rdi instruction with RIP-relative addressing" << std::endl;
            std::cout << "Target address: 0x" << std::hex << targetAddress << std::endl;
            
            return targetAddress;
        }

        std::cout << "Unknown instruction pattern at 0x" << std::hex << instructionAddress << std::endl;
        std::cout << "Bytes: ";
        for (int i = 0; i < 8; ++i) {
            std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(instruction[i]) << " ";
        }
        std::cout << std::endl;
        
        return 0;
    }

    uintptr_t FindWorldChrMan() {
        // From the cheat table: "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88"
        std::string pattern = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88";
        
        uintptr_t instructionAddress = AOBScan(pattern);
        if (instructionAddress == 0) {
            return 0;
        }
        
        uintptr_t pointerAddress = ExtractPointerFromInstruction(instructionAddress);
        if (pointerAddress == 0) {
            return 0;
        }
        
        // Read the actual pointer value
        uintptr_t worldChrManAddress;
        SIZE_T bytesRead;
        if (!ReadProcessMemory(processHandle, 
                             reinterpret_cast<LPCVOID>(pointerAddress), 
                             &worldChrManAddress, 
                             sizeof(worldChrManAddress), 
                             &bytesRead)) {
            std::cout << "Failed to read WorldChrMan pointer!" << std::endl;
            return 0;
        }
        
        std::cout << "WorldChrMan found at: 0x" << std::hex << worldChrManAddress << std::endl;
        return worldChrManAddress;
    }

    // Read a value at an address with error checking
    template<typename T>
    bool ReadValue(uintptr_t address, T& value) {
        SIZE_T bytesRead;
        return ReadProcessMemory(processHandle, 
                               reinterpret_cast<LPCVOID>(address), 
                               &value, 
                               sizeof(T), 
                               &bytesRead) && bytesRead == sizeof(T);
    }

    // Write a value at an address with error checking
    template<typename T>
    bool WriteValue(uintptr_t address, const T& value) {
        SIZE_T bytesWritten;
        return WriteProcessMemory(processHandle, 
                                reinterpret_cast<LPVOID>(address), 
                                &value, 
                                sizeof(T), 
                                &bytesWritten) && bytesWritten == sizeof(T);
    }

    // Resolve a pointer chain with offsets
    // The correct flow: add offset to current address, THEN read the pointer at that location
    uintptr_t ResolvePointerChain(uintptr_t baseAddress, const std::vector<uintptr_t>& offsets) {
        uintptr_t currentAddress = baseAddress;
        
        std::cout << "Starting pointer chain resolution:" << std::endl;
        std::cout << "Base: 0x" << std::hex << currentAddress << std::endl;
        
        for (size_t i = 0; i < offsets.size(); ++i) {
            if (currentAddress == 0) {
                std::cout << "Null pointer encountered at level " << i << std::endl;
                return 0;
            }
            
            // First add the offset to current address
            uintptr_t addressToRead = currentAddress + offsets[i];
            std::cout << "0x" << std::hex << currentAddress << " + 0x" << offsets[i] << " = 0x" << addressToRead;
            
            // Then read the pointer at that address
            uintptr_t nextAddress;
            if (!ReadValue(addressToRead, nextAddress)) {
                std::cout << " -> Failed to read pointer" << std::endl;
                return 0;
            }
            
            std::cout << " -> 0x" << std::hex << nextAddress << std::endl;
            currentAddress = nextAddress;
        }
        
        std::cout << "Final address: 0x" << std::hex << currentAddress << std::endl;
        return currentAddress;
    }

    // Read health value using the pointer chain from your screenshot
    bool ReadHealth(uintptr_t worldChrMan, int32_t& healthValue) {
        // Reading from your screenshot bottom to top: 10EF8 -> 0*10 -> 190 -> 0 -> 138
        // The last step should read the final value, not treat it as another pointer
        std::vector<uintptr_t> healthOffsets = {0x10EF8, 0x0, 0x190, 0x0, 0x138};
        
        uintptr_t healthAddress = ResolvePointerChain(worldChrMan, healthOffsets);
        if (healthAddress == 0) {
            std::cout << "Failed to resolve health pointer chain" << std::endl;
            return false;
        }
        
        // The final address IS the health value, not a pointer to it
        // Extract the lower 32 bits as the health value
        healthValue = static_cast<int32_t>(healthAddress & 0xFFFFFFFF);
        
        std::cout << "Health value: " << std::dec << healthValue << std::endl;
        return true;
    }

    // Alternative method: Stop one step earlier and read the final value properly
    bool ReadHealthCorrect(uintptr_t worldChrMan, int32_t& healthValue) {
        // Stop at the second-to-last offset and read the final value
        std::vector<uintptr_t> healthOffsets = {0x10EF8, 0x0, 0x190, 0x0};
        
        uintptr_t penultimateAddress = ResolvePointerChain(worldChrMan, healthOffsets);
        if (penultimateAddress == 0) {
            std::cout << "Failed to resolve health pointer chain" << std::endl;
            return false;
        }
        
        // Now read the 4-byte integer at penultimateAddress + 0x138
        uintptr_t finalHealthAddress = penultimateAddress + 0x138;
        std::cout << "Reading health from final address: 0x" << std::hex << finalHealthAddress << std::endl;
        
        if (!ReadValue(finalHealthAddress, healthValue)) {
            std::cout << "Failed to read health value at 0x" << std::hex << finalHealthAddress << std::endl;
            return false;
        }
        
        std::cout << "Health value: " << std::dec << healthValue << std::endl;
        return true;
    }

    // Write health value using the corrected method
    bool WriteHealth(uintptr_t worldChrMan, int32_t newHealthValue) {
        std::vector<uintptr_t> healthOffsets = {0x10EF8, 0x0, 0x190, 0x0};
        
        uintptr_t penultimateAddress = ResolvePointerChain(worldChrMan, healthOffsets);
        if (penultimateAddress == 0) {
            return false;
        }
        
        uintptr_t finalHealthAddress = penultimateAddress + 0x138;
        
        if (!WriteValue(finalHealthAddress, newHealthValue)) {
            std::cout << "Failed to write health value" << std::endl;
            return false;
        }
        
        std::cout << "Successfully wrote health value: " << std::dec << newHealthValue << std::endl;
        return true;
    }

    // Generic function to read any value through a pointer chain
    template<typename T>
    bool ReadThroughPointerChain(uintptr_t baseAddress, const std::vector<uintptr_t>& offsets, T& value) {
        uintptr_t finalAddress = ResolvePointerChain(baseAddress, offsets);
        if (finalAddress == 0) {
            return false;
        }
        return ReadValue(finalAddress, value);
    }

    // Generic function to write any value through a pointer chain
    template<typename T>
    bool WriteThroughPointerChain(uintptr_t baseAddress, const std::vector<uintptr_t>& offsets, const T& value) {
        uintptr_t finalAddress = ResolvePointerChain(baseAddress, offsets);
        if (finalAddress == 0) {
            return false;
        }
        return WriteValue(finalAddress, value);
    }

    // Additional base addresses from the cheat table
    void FindAllBases() {
        struct BasePattern {
            std::string name;
            std::string pattern;
            int offset;
        };
        
        std::vector<BasePattern> patterns = {
            {"GameDataMan", "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3", 0},
            {"GameMan", "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3", 0},
            {"FieldArea", "48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF", 0},
            {"MsgRepository", "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75", 0},
            {"WorldChrMan", "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88", 0}
        };
        
        std::cout << "\n=== Scanning for all base addresses ===" << std::endl;
        
        for (const auto& base : patterns) {
            std::cout << "\nSearching for " << base.name << "..." << std::endl;
            
            uintptr_t instructionAddress = AOBScan(base.pattern);
            if (instructionAddress != 0) {
                uintptr_t pointerAddress = ExtractPointerFromInstruction(instructionAddress + base.offset);
                if (pointerAddress != 0) {
                    uintptr_t actualAddress;
                    SIZE_T bytesRead;
                    if (ReadProcessMemory(processHandle, 
                                        reinterpret_cast<LPCVOID>(pointerAddress), 
                                        &actualAddress, 
                                        sizeof(actualAddress), 
                                        &bytesRead)) {
                        std::cout << base.name << " base: 0x" << std::hex << actualAddress << std::endl;
                    }
                }
            }
        }
    }
};

int main() {
    EldenRingMemory memory;
    
    if (!memory.Initialize()) {
        system("pause");
        return 1;
    }
    
    std::cout << "\n=== Finding WorldChrMan ===" << std::endl;
    uintptr_t worldChrMan = memory.FindWorldChrMan();
    
    if (worldChrMan == 0) {
        std::cout << "FAILED: Could not find WorldChrMan" << std::endl;
        system("pause");
        return 1;
    }
    
    std::cout << "SUCCESS: WorldChrMan found at 0x" << std::hex << worldChrMan << std::endl;
    
    // Test pointer chain resolution
    std::cout << "\n=== Testing Pointer Chain Resolution ===" << std::endl;
    
    // Read current health
    int32_t currentHealth;
    if (memory.ReadHealth(worldChrMan, currentHealth)) {
        std::cout << "Current health: " << std::dec << currentHealth << std::endl;
        
        // Demonstrate modifying health (BE CAREFUL!)
        std::cout << "\nWould you like to modify health? (y/n): ";
        char choice;
        std::cin >> choice;
        
        if (choice == 'y' || choice == 'Y') {
            std::cout << "Enter new health value: ";
            int32_t newHealth;
            std::cin >> newHealth;
            
            if (memory.WriteHealth(worldChrMan, newHealth)) {
                std::cout << "Health modified successfully!" << std::endl;
                
                // Verify the change
                int32_t verifyHealth;
                if (memory.ReadHealth(worldChrMan, verifyHealth)) {
                    std::cout << "Verified health: " << std::dec << verifyHealth << std::endl;
                }
            }
        }
    }
    
    // Demonstrate generic pointer chain reading
    std::cout << "\n=== Generic Pointer Chain Example ===" << std::endl;
    
    // You can add more pointer chains here for other values
    // Example for a different stat (you'd need to find the actual offsets):
    std::vector<uintptr_t> exampleOffsets = {0x138, 0x10}; // Example offsets
    float exampleValue;
    if (memory.ReadThroughPointerChain(worldChrMan, exampleOffsets, exampleValue)) {
        std::cout << "Example float value: " << exampleValue << std::endl;
    }
    
    // Find all other base addresses
    memory.FindAllBases();
    
    std::cout << "\nPress any key to exit..." << std::endl;
    system("pause");
    return 0;
}