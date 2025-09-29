#include "process_attribute.h"
#include "process_memory.h"
#include "server.h"
#include "utils.h"
#include <iostream>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>

int main() {
    std::string processName;
    std::map<std::string, ProcessAttribute> processAttributes;

    if (!IsRunAsAdmin()) {
        std::cout << "ERROR: Must run as Administrator!" << std::endl;
        system("pause");
        return 1;
    }
    // TODO: Get attribute file from command line
    GetProcessInfoFromTOML("../attributes.toml", &processName, &processAttributes);
    // PrintProcessAttributes(attributes);
    PrintProcessAttributes(processAttributes);
    std::cout << "Process name: " << processName << std::endl;

    ProcessMemory memory(processName, processAttributes);
    if (memory.Initialize()) {
        std::cout << "Process memory initialized successfully!" << std::endl;
    } else {
        std::cout << "Failed to initialize process memory!" << std::endl;
    }

    std::cout << "Starting gRPC Variable Service Server..." << std::endl;
    RunServer(&memory);
    return 0;

    // int32_t Hp;
    // while (true) {
    //     memory.ReadInt32(HpAddress, Hp);
    //     std::cout << "Hp: " << std::dec << Hp << std::endl;
    //     Sleep(1000);
    // }
    // system("pause");
    return 0;
}
