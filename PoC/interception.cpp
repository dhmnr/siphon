// main.cpp
#include "interception.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <windows.h>

// Scancode definitions (hardware scancodes)
#define SCANCODE_W 0x11
#define SCANCODE_A 0x1E
#define SCANCODE_S 0x1F
#define SCANCODE_D 0x20
#define SCANCODE_SPACE 0x39
#define SCANCODE_F 0x21
#define SCANCODE_E 0x12
#define SCANCODE_ESC 0x01

class EldenRingController {
  private:
    HWND gameWindow;
    InterceptionContext context;
    InterceptionDevice keyboard;

    // Callback for EnumWindows
    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
        char title[256];
        char className[256];

        GetWindowTextA(hwnd, title, sizeof(title));
        GetClassNameA(hwnd, className, sizeof(className));

        // Check if this is Elden Ring (starts with "ELDEN RING")
        if (strncmp(title, "ELDEN RING", 10) == 0 || strncmp(className, "ELDEN RING", 10) == 0) {
            if (IsWindowVisible(hwnd)) {
                HWND *pGameWindow = reinterpret_cast<HWND *>(lParam);
                *pGameWindow = hwnd;
                return FALSE; // Stop enumeration
            }
        }
        return TRUE; // Continue enumeration
    }

  public:
    EldenRingController() : gameWindow(NULL), context(nullptr), keyboard(0) {
        // Find Elden Ring window
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&gameWindow));

        if (!gameWindow) {
            std::cerr << "Elden Ring window not found! Make sure the game is running." << std::endl;
        } else {
            std::cout << "Elden Ring window found!" << std::endl;
        }

        // Initialize Interception
        context = interception_create_context();

        if (context == nullptr) {
            std::cerr << "Failed to create Interception context!" << std::endl;
            std::cerr << "Make sure Interception driver is installed and running." << std::endl;
            return;
        }

        // Find the first keyboard device
        for (InterceptionDevice device = INTERCEPTION_KEYBOARD(0);
             device <= INTERCEPTION_KEYBOARD(INTERCEPTION_MAX_KEYBOARD - 1); device++) {
            if (interception_is_keyboard(device)) {
                keyboard = device;
                break;
            }
        }

        if (keyboard == 0) {
            std::cerr << "No keyboard device found!" << std::endl;
            return;
        }

        std::cout << "Interception initialized successfully!" << std::endl;
    }

    ~EldenRingController() {
        if (context) {
            interception_destroy_context(context);
        }
    }

    bool IsInitialized() const {
        return gameWindow != nullptr && context != nullptr && keyboard != 0;
    }

    // Ensure game window is in focus
    bool BringToFocus() {
        if (!gameWindow)
            return false;

        // Check if already focused
        if (GetForegroundWindow() == gameWindow) {
            return true;
        }
        std::cout << " window to focus " << std::hex << gameWindow << std::endl;

        // Bring to foreground
        SetForegroundWindow(gameWindow);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        return GetForegroundWindow() == gameWindow;
    }

    void PressKey(unsigned short scancode) {
        if (!context || keyboard == 0)
            return;

        InterceptionKeyStroke stroke;
        stroke.code = scancode;
        stroke.state = INTERCEPTION_KEY_DOWN;
        stroke.information = 0;

        interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
    }

    void ReleaseKey(unsigned short scancode) {
        if (!context || keyboard == 0)
            return;

        InterceptionKeyStroke stroke;
        stroke.code = scancode;
        stroke.state = INTERCEPTION_KEY_UP;
        stroke.information = 0;

        interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
    }

    void TapKey(unsigned short scancode, int holdMs = 500) {
        PressKey(scancode);
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        ReleaseKey(scancode);
    }

    // Elden Ring specific actions
    void MoveForward(bool hold = false) {
        if (hold)
            PressKey(SCANCODE_W);
        else
            TapKey(SCANCODE_W);
    }

    void MoveBackward(bool hold = false) {
        if (hold)
            PressKey(SCANCODE_S);
        else
            TapKey(SCANCODE_S);
    }

    void StrafeLeft(bool hold = false) {
        if (hold)
            PressKey(SCANCODE_A);
        else
            TapKey(SCANCODE_A);
    }

    void StrafeRight(bool hold = false) {
        if (hold)
            PressKey(SCANCODE_D);
        else
            TapKey(SCANCODE_D);
    }

    void Dodge() { TapKey(SCANCODE_SPACE); }

    void Jump() { TapKey(SCANCODE_F); }

    void Interact() { TapKey(SCANCODE_E); }

    void ReleaseAll() {
        ReleaseKey(SCANCODE_W);
        ReleaseKey(SCANCODE_A);
        ReleaseKey(SCANCODE_S);
        ReleaseKey(SCANCODE_D);
        ReleaseKey(SCANCODE_SPACE);
    }
};

int main() {
    try {
        EldenRingController controller;

        if (!controller.IsInitialized()) {
            std::cerr << "Failed to initialize controller!" << std::endl;
            std::cerr << "Make sure:" << std::endl;
            std::cerr << "1. Elden Ring is running" << std::endl;
            std::cerr << "2. Interception driver is installed (install-interception.exe /install)"
                      << std::endl;
            return 1;
        }

        std::cout << "Controller initialized!" << std::endl;
        std::cout << "Starting test sequence in 3 seconds..." << std::endl;
        std::cout << "Switch to Elden Ring!" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // Bring window to focus
        if (!controller.BringToFocus()) {
            std::cout << "Warning: Could not bring window to focus, trying anyway..." << std::endl;
        } else {
            std::cout << "Window focused!" << std::endl;
        }

        // Test sequence
        std::cout << "Test 1: Pressing W (move forward)" << std::endl;
        controller.TapKey(SCANCODE_W, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "Test 2: Holding W for 1 second" << std::endl;
        controller.MoveForward(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10000));
        controller.ReleaseAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "Test 3: Jump (F key)" << std::endl;
        controller.Jump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        std::cout << "Test 4: Dodge (Space)" << std::endl;
        controller.Dodge();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        std::cout << "Test complete!" << std::endl;

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}