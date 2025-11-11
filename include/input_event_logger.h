#pragma once

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Structure for a single input event
struct InputEvent {
    int64_t timestampUs;     // Microseconds since epoch
    std::string eventType;   // "KEY_DOWN", "KEY_UP", "MOUSE_DOWN", "MOUSE_UP", "MOUSE_MOVE"
    std::string keyOrButton; // Key name or button name
    int mouseX;              // For mouse move events
    int mouseY;              // For mouse move events
};

class InputEventLogger {
  private:
    // State
    std::atomic<bool> isLogging_;
    std::atomic<bool> shouldStop_;
    std::thread writerThread_;
    std::thread hookThread_;
    std::atomic<bool> hooksReady_;

    // Output
    std::string outputFilePath_;
    std::ofstream outputFile_;
    std::mutex fileMutex_;

    // Event buffer
    std::vector<InputEvent> eventBuffer_;
    std::mutex bufferMutex_;
    size_t maxBufferSize_;

    // Hooks
    HHOOK keyboardHook_;
    HHOOK mouseHook_;
    static InputEventLogger *instance_;

    // Private methods
    void HookMessageLoop();
    void WriterLoop();
    void FlushBuffer();
    std::string VirtualKeyToString(DWORD vkCode);
    int64_t GetCurrentTimestampUs();

    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);

  public:
    InputEventLogger();
    ~InputEventLogger();

    bool StartLogging(const std::string &outputFilePath);
    bool StopLogging();
    bool IsLogging() const { return isLogging_; }
    size_t GetEventCount() const;
};
