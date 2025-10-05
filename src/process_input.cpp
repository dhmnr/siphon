
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <windows.h>

#include "process_input.h"
#include "utils.h"
#include <spdlog/spdlog.h>


std::map<std::string, unsigned short> scancodeMap = {
    {"W", 0x11},     {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20},
    {"SPACE", 0x39}, {"F", 0x21}, {"E", 0x12}, {"ESC", 0x01},
};

ProcessInput::ProcessInput() : context(nullptr), keyboard(0) {}

ProcessInput::~ProcessInput() {
    if (context) {
        interception_destroy_context(context);
    }
}

bool ProcessInput::Initialize(HWND processWindow) {
    this->processWindow = processWindow;
    if (!processWindow) {
        spdlog::error("Process window not found! Make sure the it is running.");
        return false;
    } else {
        spdlog::info("Process window found! HWND: 0x{:X}",
                     reinterpret_cast<uintptr_t>(processWindow));
    }

    // Initialize Interception
    context = interception_create_context();

    if (context == nullptr) {
        spdlog::error("Failed to create Interception context!");
        spdlog::error("Make sure Interception driver is installed and running.");
        return false;
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
        return false;
    }

    spdlog::info("Process input initialized successfully!");
    return true;
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

bool ProcessInput::TapKey(std::string key, int holdMs) {

    spdlog::info("Tapping key: {} for {}ms", key, holdMs);
    if (!context || keyboard == 0) {
        spdlog::error("Failed to initialize controller!");
        spdlog::error(
            "Make sure Interception driver is installed (install-interception.exe /install)");
        return false;
    }
    BringToFocus(processWindow);
    // std::this_thread::sleep_for(std::chrono::milliseconds(800));
    PressKey(key);
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    ReleaseKey(key);
    return true;
}
