#include "input_event_logger.h"
#include <map>
#include <spdlog/spdlog.h>

// Static instance for hook callbacks
InputEventLogger *InputEventLogger::instance_ = nullptr;

InputEventLogger::InputEventLogger()
    : isLogging_(false), shouldStop_(false), hooksReady_(false), keyboardHook_(nullptr),
      mouseHook_(nullptr), maxBufferSize_(10000) {
    instance_ = this;
}

InputEventLogger::~InputEventLogger() {
    if (isLogging_) {
        StopLogging();
    }

    if (instance_ == this) {
        instance_ = nullptr;
    }
}

int64_t InputEventLogger::GetCurrentTimestampUs() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

bool InputEventLogger::StartLogging(const std::string &outputFilePath) {
    if (isLogging_) {
        spdlog::warn("Input logging already in progress");
        return false;
    }

    outputFilePath_ = outputFilePath;

    // Open output file
    outputFile_.open(outputFilePath_, std::ios::out | std::ios::trunc);
    if (!outputFile_.is_open()) {
        spdlog::error("Failed to open input log file: {}", outputFilePath_);
        return false;
    }

    // Write CSV header
    outputFile_ << "timestamp_us,event_type,key_or_button,mouse_x,mouse_y\n";
    outputFile_.flush();

    // Clear event buffer
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        eventBuffer_.clear();
    }

    // Start hook thread
    hooksReady_ = false;
    hookThread_ = std::thread(&InputEventLogger::HookMessageLoop, this);

    // Wait for hooks to be ready
    auto startTime = std::chrono::steady_clock::now();
    while (!hooksReady_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(5)) {
            spdlog::error("Timeout waiting for input hooks to initialize");
            if (hookThread_.joinable()) {
                PostThreadMessage(GetThreadId(hookThread_.native_handle()), WM_QUIT, 0, 0);
                hookThread_.join();
            }
            outputFile_.close();
            return false;
        }
    }

    // Start writer thread
    shouldStop_ = false;
    isLogging_ = true;
    writerThread_ = std::thread(&InputEventLogger::WriterLoop, this);

    spdlog::info("Input event logging started: {}", outputFilePath_);
    return true;
}

bool InputEventLogger::StopLogging() {
    if (!isLogging_) {
        spdlog::warn("Input logging not in progress");
        return false;
    }

    spdlog::info("Stopping input event logging...");
    shouldStop_ = true;
    isLogging_ = false;

    // Stop writer thread
    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    // Stop hook thread
    if (hookThread_.joinable()) {
        PostThreadMessage(GetThreadId(hookThread_.native_handle()), WM_QUIT, 0, 0);
        hookThread_.join();
    }

    // Final flush
    FlushBuffer();

    // Close file
    if (outputFile_.is_open()) {
        outputFile_.close();
    }

    spdlog::info("Input event logging stopped");
    return true;
}

void InputEventLogger::HookMessageLoop() {
    spdlog::info("Input hook message loop thread started");

    // Install keyboard hook
    keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandle(NULL), 0);
    if (!keyboardHook_) {
        spdlog::error("Failed to create keyboard hook: {}", GetLastError());
        hooksReady_ = true;
        return;
    }

    // Install mouse hook
    mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, GetModuleHandle(NULL), 0);
    if (!mouseHook_) {
        spdlog::error("Failed to create mouse hook: {}", GetLastError());
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
        hooksReady_ = true;
        return;
    }

    spdlog::info("Input hooks installed successfully");
    hooksReady_ = true;

    // Message loop - required for hooks to receive events
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup hooks when message loop exits
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }

    spdlog::info("Input hook message loop thread stopped");
}

void InputEventLogger::WriterLoop() {
    spdlog::info("Input event writer thread started");

    while (!shouldStop_) {
        // Flush buffer every 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        FlushBuffer();
    }

    spdlog::info("Input event writer thread stopped");
}

void InputEventLogger::FlushBuffer() {
    std::vector<InputEvent> eventsToWrite;

    // Swap out the buffer
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (eventBuffer_.empty()) {
            return;
        }
        eventsToWrite.swap(eventBuffer_);
    }

    // Write to file
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (!outputFile_.is_open()) {
            return;
        }

        for (const auto &event : eventsToWrite) {
            outputFile_ << event.timestampUs << "," << event.eventType << "," << event.keyOrButton
                        << "," << event.mouseX << "," << event.mouseY << "\n";
        }
        outputFile_.flush();
    }

    spdlog::debug("Flushed {} input events to disk", eventsToWrite.size());
}

size_t InputEventLogger::GetEventCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(bufferMutex_));
    return eventBuffer_.size();
}

// Keyboard hook callback
LRESULT CALLBACK InputEventLogger::KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && instance_ && instance_->isLogging_) {
        KBDLLHOOKSTRUCT *pKeyBoard = (KBDLLHOOKSTRUCT *)lParam;

        InputEvent event;
        event.timestampUs = instance_->GetCurrentTimestampUs();
        event.keyOrButton = instance_->VirtualKeyToString(pKeyBoard->vkCode);
        event.mouseX = 0;
        event.mouseY = 0;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            event.eventType = "KEY_DOWN";
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            event.eventType = "KEY_UP";
        } else {
            // Unknown event type, skip
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        // Add to buffer
        {
            std::lock_guard<std::mutex> lock(instance_->bufferMutex_);
            instance_->eventBuffer_.push_back(event);

            // If buffer is getting large, warn (writer should flush more frequently)
            if (instance_->eventBuffer_.size() > instance_->maxBufferSize_) {
                spdlog::warn("Input event buffer overflow! Size: {}",
                             instance_->eventBuffer_.size());
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Mouse hook callback
LRESULT CALLBACK InputEventLogger::MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && instance_ && instance_->isLogging_) {
        MSLLHOOKSTRUCT *pMouse = (MSLLHOOKSTRUCT *)lParam;

        InputEvent event;
        event.timestampUs = instance_->GetCurrentTimestampUs();
        event.mouseX = pMouse->pt.x;
        event.mouseY = pMouse->pt.y;

        switch (wParam) {
        case WM_LBUTTONDOWN:
            event.eventType = "MOUSE_DOWN";
            event.keyOrButton = "LEFT";
            break;
        case WM_LBUTTONUP:
            event.eventType = "MOUSE_UP";
            event.keyOrButton = "LEFT";
            break;
        case WM_RBUTTONDOWN:
            event.eventType = "MOUSE_DOWN";
            event.keyOrButton = "RIGHT";
            break;
        case WM_RBUTTONUP:
            event.eventType = "MOUSE_UP";
            event.keyOrButton = "RIGHT";
            break;
        case WM_MBUTTONDOWN:
            event.eventType = "MOUSE_DOWN";
            event.keyOrButton = "MIDDLE";
            break;
        case WM_MBUTTONUP:
            event.eventType = "MOUSE_UP";
            event.keyOrButton = "MIDDLE";
            break;
        case WM_XBUTTONDOWN: {
            event.eventType = "MOUSE_DOWN";
            if (HIWORD(pMouse->mouseData) == XBUTTON1) {
                event.keyOrButton = "BUTTON4";
            } else if (HIWORD(pMouse->mouseData) == XBUTTON2) {
                event.keyOrButton = "BUTTON5";
            }
            break;
        }
        case WM_XBUTTONUP: {
            event.eventType = "MOUSE_UP";
            if (HIWORD(pMouse->mouseData) == XBUTTON1) {
                event.keyOrButton = "BUTTON4";
            } else if (HIWORD(pMouse->mouseData) == XBUTTON2) {
                event.keyOrButton = "BUTTON5";
            }
            break;
        }
        case WM_MOUSEMOVE:
            event.eventType = "MOUSE_MOVE";
            event.keyOrButton = "MOVE";
            break;
        case WM_MOUSEWHEEL:
            event.eventType = "MOUSE_WHEEL";
            event.keyOrButton = "WHEEL";
            event.mouseX = GET_WHEEL_DELTA_WPARAM(pMouse->mouseData);
            break;
        default:
            // Unknown mouse event, skip
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        // Add to buffer
        {
            std::lock_guard<std::mutex> lock(instance_->bufferMutex_);
            instance_->eventBuffer_.push_back(event);

            if (instance_->eventBuffer_.size() > instance_->maxBufferSize_) {
                spdlog::warn("Input event buffer overflow! Size: {}",
                             instance_->eventBuffer_.size());
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Convert Windows Virtual Key Code to string
std::string InputEventLogger::VirtualKeyToString(DWORD vkCode) {
    // Map virtual key codes to key names
    static std::map<DWORD, std::string> vkMap = {
        {VK_ESCAPE, "ESC"},
        {VK_BACK, "BACKSPACE"},
        {VK_TAB, "TAB"},
        {VK_RETURN, "ENTER"},
        {VK_SPACE, "SPACE"},
        {VK_CAPITAL, "CAPSLOCK"},
        {VK_NUMLOCK, "NUMLOCK"},
        {VK_SCROLL, "SCROLLLOCK"},

        // Modifiers
        {VK_LSHIFT, "LEFT_SHIFT"},
        {VK_RSHIFT, "RIGHT_SHIFT"},
        {VK_LCONTROL, "LEFT_CTRL"},
        {VK_RCONTROL, "RIGHT_CTRL"},
        {VK_LMENU, "LEFT_ALT"},
        {VK_RMENU, "RIGHT_ALT"},

        // Function keys
        {VK_F1, "F1"},
        {VK_F2, "F2"},
        {VK_F3, "F3"},
        {VK_F4, "F4"},
        {VK_F5, "F5"},
        {VK_F6, "F6"},
        {VK_F7, "F7"},
        {VK_F8, "F8"},
        {VK_F9, "F9"},
        {VK_F10, "F10"},
        {VK_F11, "F11"},
        {VK_F12, "F12"},

        // Letters (A-Z)
        {0x41, "A"},
        {0x42, "B"},
        {0x43, "C"},
        {0x44, "D"},
        {0x45, "E"},
        {0x46, "F"},
        {0x47, "G"},
        {0x48, "H"},
        {0x49, "I"},
        {0x4A, "J"},
        {0x4B, "K"},
        {0x4C, "L"},
        {0x4D, "M"},
        {0x4E, "N"},
        {0x4F, "O"},
        {0x50, "P"},
        {0x51, "Q"},
        {0x52, "R"},
        {0x53, "S"},
        {0x54, "T"},
        {0x55, "U"},
        {0x56, "V"},
        {0x57, "W"},
        {0x58, "X"},
        {0x59, "Y"},
        {0x5A, "Z"},

        // Numbers (0-9)
        {0x30, "0"},
        {0x31, "1"},
        {0x32, "2"},
        {0x33, "3"},
        {0x34, "4"},
        {0x35, "5"},
        {0x36, "6"},
        {0x37, "7"},
        {0x38, "8"},
        {0x39, "9"},

        // Numpad
        {VK_NUMPAD0, "KEYPAD_0"},
        {VK_NUMPAD1, "KEYPAD_1"},
        {VK_NUMPAD2, "KEYPAD_2"},
        {VK_NUMPAD3, "KEYPAD_3"},
        {VK_NUMPAD4, "KEYPAD_4"},
        {VK_NUMPAD5, "KEYPAD_5"},
        {VK_NUMPAD6, "KEYPAD_6"},
        {VK_NUMPAD7, "KEYPAD_7"},
        {VK_NUMPAD8, "KEYPAD_8"},
        {VK_NUMPAD9, "KEYPAD_9"},

        // Symbols
        {VK_OEM_MINUS, "MINUS"},
        {VK_OEM_PLUS, "EQUALS"},
        {VK_OEM_4, "LEFT_BRACKET"},
        {VK_OEM_6, "RIGHT_BRACKET"},
        {VK_OEM_1, "SEMICOLON"},
        {VK_OEM_7, "APOSTROPHE"},
        {VK_OEM_3, "GRAVE"},
        {VK_OEM_5, "BACKSLASH"},
        {VK_OEM_COMMA, "COMMA"},
        {VK_OEM_PERIOD, "PERIOD"},
        {VK_OEM_2, "SLASH"},

        // Arrow keys
        {VK_UP, "UP"},
        {VK_DOWN, "DOWN"},
        {VK_LEFT, "LEFT"},
        {VK_RIGHT, "RIGHT"},

        // Other common keys
        {VK_INSERT, "INSERT"},
        {VK_DELETE, "DELETE"},
        {VK_HOME, "HOME"},
        {VK_END, "END"},
        {VK_PRIOR, "PAGE_UP"},
        {VK_NEXT, "PAGE_DOWN"},
    };

    auto it = vkMap.find(vkCode);
    if (it != vkMap.end()) {
        return it->second;
    }

    return "UNKNOWN_" + std::to_string(vkCode);
}
