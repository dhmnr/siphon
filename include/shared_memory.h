#pragma once
#include <Windows.h>

struct TargetedNpcInfo {
    void *npcPointer;      // NPC pointer captured by hook
    uintptr_t hookAddress; // Address to hook (set by executable)
    bool hookReady;        // Signal DLL to start hooking
};

class SharedMemory {
  public:
    static constexpr const char *SHARED_MEM_NAME = "EldenRingNPCPointer";
    static constexpr size_t SHARED_MEM_SIZE = sizeof(TargetedNpcInfo);

    HANDLE hMapFile = nullptr;
    TargetedNpcInfo *data = nullptr;

    bool CreateShared() {
        hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      SHARED_MEM_SIZE, SHARED_MEM_NAME);

        if (!hMapFile) {
            return false;
        }

        data =
            (TargetedNpcInfo *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_SIZE);

        if (data) {
            ZeroMemory(data, SHARED_MEM_SIZE);
        }

        return data != nullptr;
    }

    bool OpenShared() {
        hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEM_NAME);

        if (!hMapFile) {
            return false;
        }

        data =
            (TargetedNpcInfo *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_MEM_SIZE);

        return data != nullptr;
    }

    void Close() {
        if (data) {
            UnmapViewOfFile(data);
            data = nullptr;
        }
        if (hMapFile) {
            CloseHandle(hMapFile);
            hMapFile = nullptr;
        }
    }

    ~SharedMemory() { Close(); }
};