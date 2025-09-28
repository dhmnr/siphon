#include <windows.h>

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