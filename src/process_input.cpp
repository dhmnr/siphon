
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <windows.h>

#include "process_input.h"
#include <spdlog/spdlog.h>

std::map<std::string, unsigned short> scancodeMap = {
    {"W", 0x11},     {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20},
    {"SPACE", 0x39}, {"F", 0x21}, {"E", 0x12}, {"ESC", 0x01},
};

ProcessInput::ProcessInput(HWND processWindow)
    : processWindow(processWindow), context(nullptr), keyboard(0) {
    // Find process window

    if (!processWindow) {
        spdlog::error("Process window not found! Make sure the it is running.");
    } else {
        spdlog::info("Process window found! HWND: 0x{:X}",
                     reinterpret_cast<uintptr_t>(processWindow));
    }

    // Initialize Interception
    context = interception_create_context();

    if (context == nullptr) {
        spdlog::error("Failed to create Interception context!");
        spdlog::error("Make sure Interception driver is installed and running.");
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
        spdlog::error("No keyboard device found!");
        return;
    }

    spdlog::info("Interception initialized successfully!");
}

ProcessInput::~ProcessInput() {
    if (context) {
        interception_destroy_context(context);
    }
}

bool ProcessInput::IsInitialized() const {
    return processWindow != nullptr && context != nullptr && keyboard != 0;
}

// Ensure game window is in focus
bool ProcessInput::BringToFocus() {
    if (!processWindow)
        return false;

    // Check if already focused
    if (GetForegroundWindow() == processWindow) {
        return true;
    }

    // Method 1: Try simple approach first
    ShowWindow(processWindow, SW_RESTORE);
    SetForegroundWindow(processWindow);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // If that worked, we're done
    if (GetForegroundWindow() == processWindow) {
        return true;
    }

    // Method 2: Simulate Alt key press to allow focus change
    // This tricks Windows into thinking user is switching windows
    keybd_event(VK_MENU, 0, 0, 0); // Alt down
    SetForegroundWindow(processWindow);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0); // Alt up

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return GetForegroundWindow() == processWindow;
}

void ProcessInput::PressKey(std::string key) {
    if (!context || keyboard == 0)
        return;

    InterceptionKeyStroke stroke;
    stroke.code = scancodeMap[key];
    stroke.state = INTERCEPTION_KEY_DOWN;
    stroke.information = 0;

    interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
}

void ProcessInput::ReleaseKey(std::string key) {
    if (!context || keyboard == 0)
        return;

    InterceptionKeyStroke stroke;
    stroke.code = scancodeMap[key];
    stroke.state = INTERCEPTION_KEY_UP;
    stroke.information = 0;

    interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
}

void ProcessInput::TapKey(std::string key, int holdMs) {

    spdlog::info("Tapping key: {} for {}ms", key, holdMs);
    if (!IsInitialized()) {
        spdlog::error("Failed to initialize controller!");
        spdlog::error("Make sure:");
        spdlog::error("1. Process is running");
        spdlog::error("2. Interception driver is installed (install-interception.exe /install)");
        return;
    }
    if (!this->BringToFocus()) {
        spdlog::warn("Warning: Could not bring window to focus, trying anyway...");
    } else {
        spdlog::info("Window focused!");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    PressKey(key);
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    ReleaseKey(key);
}
