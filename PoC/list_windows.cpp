#include <iostream>
#include <windows.h>
#include <string>

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    wchar_t windowTitle[512];
    wchar_t className[256];
    
    // Only show visible windows with titles
    if (IsWindowVisible(hwnd) && GetWindowTextW(hwnd, windowTitle, 512) > 0) {
        GetClassNameW(hwnd, className, 256);
        
        // Convert to console output
        wprintf(L"Title: %s\n", windowTitle);
        wprintf(L"Class: %s\n", className);
        wprintf(L"HWND: %p\n\n", hwnd);
    }
    return TRUE;
}

int main() {
    wprintf(L"=== All Visible Windows ===\n\n");
    
    EnumWindows(EnumWindowsProc, 0);
    
    wprintf(L"Done listing windows.\n");
    wprintf(L"Press Enter to exit...");
    getchar();
    return 0;
}