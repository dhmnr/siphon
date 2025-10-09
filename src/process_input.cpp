#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <windows.h>

#include "process_input.h"
#include "utils.h"
#include <spdlog/spdlog.h>

std::map<std::string, unsigned short> scancodeMap = {
    // Letters (A-Z)
    {"A", 0x1E},
    {"B", 0x30},
    {"C", 0x2E},
    {"D", 0x20},
    {"E", 0x12},
    {"F", 0x21},
    {"G", 0x22},
    {"H", 0x23},
    {"I", 0x17},
    {"J", 0x24},
    {"K", 0x25},
    {"L", 0x26},
    {"M", 0x32},
    {"N", 0x31},
    {"O", 0x18},
    {"P", 0x19},
    {"Q", 0x10},
    {"R", 0x13},
    {"S", 0x1F},
    {"T", 0x14},
    {"U", 0x16},
    {"V", 0x2F},
    {"W", 0x11},
    {"X", 0x2D},
    {"Y", 0x15},
    {"Z", 0x2C},

    // Numbers (0-9)
    {"0", 0x0B},
    {"1", 0x02},
    {"2", 0x03},
    {"3", 0x04},
    {"4", 0x05},
    {"5", 0x06},
    {"6", 0x07},
    {"7", 0x08},
    {"8", 0x09},
    {"9", 0x0A},

    // Function Keys (F1-F12)
    {"F1", 0x3B},
    {"F2", 0x3C},
    {"F3", 0x3D},
    {"F4", 0x3E},
    {"F5", 0x3F},
    {"F6", 0x40},
    {"F7", 0x41},
    {"F8", 0x42},
    {"F9", 0x43},
    {"F10", 0x44},
    {"F11", 0x57},
    {"F12", 0x58},

    // Special Keys
    {"ESC", 0x01},
    {"BACKSPACE", 0x0E},
    {"TAB", 0x0F},
    {"ENTER", 0x1C},
    {"SPACE", 0x39},
    {"CAPSLOCK", 0x3A},
    {"NUMLOCK", 0x45},
    {"SCROLLLOCK", 0x46},

    // Modifiers
    {"LEFT_SHIFT", 0x2A},
    {"RIGHT_SHIFT", 0x36},
    {"LEFT_CTRL", 0x1D},
    {"LEFT_ALT", 0x38},

    // Symbols
    {"MINUS", 0x0C},         // -
    {"EQUALS", 0x0D},        // =
    {"LEFT_BRACKET", 0x1A},  // [
    {"RIGHT_BRACKET", 0x1B}, // ]
    {"SEMICOLON", 0x27},     // ;
    {"APOSTROPHE", 0x28},    // '
    {"GRAVE", 0x29},         // `
    {"BACKSLASH", 0x2B},     //
    {"COMMA", 0x33},         // ,
    {"PERIOD", 0x34},        // .
    {"SLASH", 0x35},         // /

    // Keypad
    {"KEYPAD_0", 0x52},
    {"KEYPAD_1", 0x4F},
    {"KEYPAD_2", 0x50},
    {"KEYPAD_3", 0x51},
    {"KEYPAD_4", 0x4B},
    {"KEYPAD_5", 0x4C},
    {"KEYPAD_6", 0x4D},
    {"KEYPAD_7", 0x47},
    {"KEYPAD_8", 0x48},
    {"KEYPAD_9", 0x49},
    {"KEYPAD_STAR", 0x37},   // *
    {"KEYPAD_PLUS", 0x4E},   // +
    {"KEYPAD_MINUS", 0x4A},  // -
    {"KEYPAD_PERIOD", 0x53}, // .
};

// Mouse button definitions
std::map<std::string, unsigned short> mouseButtonMap = {
    {"LEFT", INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN},     // 0x001
    {"RIGHT", INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN},   // 0x004
    {"MIDDLE", INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN}, // 0x010
    {"BUTTON4", INTERCEPTION_MOUSE_BUTTON_4_DOWN},     // 0x040
    {"BUTTON5", INTERCEPTION_MOUSE_BUTTON_5_DOWN},     // 0x100
};

std::map<std::string, unsigned short> mouseButtonReleaseMap = {
    {"LEFT", INTERCEPTION_MOUSE_LEFT_BUTTON_UP},     // 0x002
    {"RIGHT", INTERCEPTION_MOUSE_RIGHT_BUTTON_UP},   // 0x008
    {"MIDDLE", INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP}, // 0x020
    {"BUTTON4", INTERCEPTION_MOUSE_BUTTON_4_UP},     // 0x080
    {"BUTTON5", INTERCEPTION_MOUSE_BUTTON_5_UP},     // 0x200
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

    for (int i = INTERCEPTION_MOUSE(0); i <= INTERCEPTION_MOUSE(9); i++) {
        if (interception_is_mouse(i)) {
            mouse = i;
            spdlog::info("Interception: Mouse device found: {}", i);
            break;
        }
    }

    if (mouse == 0) {
        spdlog::error("No mouse device found!");
        return false;
    }

    spdlog::info("Process input initialized successfully!");
    return true;
}

void ProcessInput::PressKey(std::string key) {
    if (!context || keyboard == 0)
        return;

    // Convert key to uppercase
    std::transform(key.begin(), key.end(), key.begin(), ::toupper);

    InterceptionKeyStroke stroke;
    stroke.code = scancodeMap[key];
    stroke.state = INTERCEPTION_KEY_DOWN;
    stroke.information = 0;
    spdlog::info("Interception: Pressing key: {}", key);
    interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
}

void ProcessInput::ReleaseKey(std::string key) {
    if (!context || keyboard == 0)
        return;

    // Convert key to uppercase
    std::transform(key.begin(), key.end(), key.begin(), ::toupper);

    InterceptionKeyStroke stroke;
    stroke.code = scancodeMap[key];
    stroke.state = INTERCEPTION_KEY_UP;
    stroke.information = 0;
    spdlog::info("Interception: Releasing key: {}", key);
    interception_send(context, keyboard, (InterceptionStroke *)&stroke, 1);
}

bool ProcessInput::TapKey(std::vector<std::string> keys, int holdMs, int delayMs) {

    std::stringstream ss;
    for (const auto &key : keys) {
        ss << key << " ";
    }
    spdlog::info("Tapping keys: {} for {}ms", ss.str(), holdMs);
    if (!context || keyboard == 0 || mouse == 0) {
        spdlog::error("Failed to initialize controller!");
        spdlog::error(
            "Make sure Interception driver is installed (install-interception.exe /install)");
        return false;
    }
    BringToFocus(processWindow);
    // std::this_thread::sleep_for(std::chrono::milliseconds(800));

    for (size_t i = 0; i < keys.size(); ++i) {
        if (mouseButtonMap.find(keys[i]) != mouseButtonMap.end()) {
            PressMouseButton(keys[i]);
        } else {
            PressKey(keys[i]);
        }
        if (i < keys.size() - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {

        if (mouseButtonMap.find(*it) != mouseButtonMap.end()) {
            ReleaseMouseButton(*it);
        } else {
            ReleaseKey(*it);
        }
        if (it != keys.rend() - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    return true;
}

// Press mouse button
void ProcessInput::PressMouseButton(std::string button) {
    if (!context || mouse == 0)
        return;

    std::transform(button.begin(), button.end(), button.begin(), ::toupper);

    InterceptionMouseStroke stroke;
    stroke.state = mouseButtonMap[button];
    stroke.flags = 0;
    stroke.rolling = 0;
    stroke.x = 0;
    stroke.y = 0;
    stroke.information = 0;

    spdlog::info("Interception: Pressing mouse button: {}", button);
    interception_send(context, mouse, (InterceptionStroke *)&stroke, 1);
}

// Release mouse button
void ProcessInput::ReleaseMouseButton(std::string button) {
    if (!context || mouse == 0)
        return;

    std::transform(button.begin(), button.end(), button.begin(), ::toupper);

    InterceptionMouseStroke stroke;
    stroke.state = mouseButtonReleaseMap[button];
    stroke.flags = 0;
    stroke.rolling = 0;
    stroke.x = 0;
    stroke.y = 0;
    stroke.information = 0;

    spdlog::info("Interception: Releasing mouse button: {}", button);
    interception_send(context, mouse, (InterceptionStroke *)&stroke, 1);
}

// Click mouse button (press + release)
bool ProcessInput::ClickMouseButton(std::string button, int delayMs) {
    if (!context || mouse == 0)
        return false;

    BringToFocus(processWindow);
    PressMouseButton(button);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    ReleaseMouseButton(button);
    return true;
}

// Move mouse (relative movement)
bool ProcessInput::MoveMouse(int deltaX, int deltaY) {
    if (!context || mouse == 0)
        return false;

    InterceptionMouseStroke stroke;
    stroke.state = 0;
    stroke.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
    stroke.rolling = 0;
    stroke.x = deltaX;
    stroke.y = deltaY;
    stroke.information = 0;

    spdlog::info("Interception: Moving mouse: dx={}, dy={}", deltaX, deltaY);
    interception_send(context, mouse, (InterceptionStroke *)&stroke, 1);
    return true;
}

// Smooth mouse movement (interpolated)
bool ProcessInput::MoveMouseSmooth(int targetX, int targetY, int steps) {

    if (!context || mouse == 0)
        return false;

    int stepX = targetX / steps;
    int stepY = targetY / steps;
    BringToFocus(processWindow);
    for (int i = 0; i < steps; i++) {
        MoveMouse(stepX, stepY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Mouse wheel scroll
bool ProcessInput::ScrollWheel(int amount) {
    if (!context || mouse == 0)
        return false;

    BringToFocus(processWindow);

    InterceptionMouseStroke stroke;
    stroke.state = INTERCEPTION_MOUSE_WHEEL; // 0x400
    stroke.flags = 0;
    stroke.rolling = amount; // Positive = up, Negative = down
    stroke.x = 0;
    stroke.y = 0;
    stroke.information = 0;

    spdlog::info("Interception: Scrolling wheel: {}", amount);
    interception_send(context, mouse, (InterceptionStroke *)&stroke, 1);
    return true;
}