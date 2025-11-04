#include "process_recorder.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <spdlog/spdlog.h>
#include <sstream>

namespace fs = std::filesystem;

// Static instance for hook callback
ProcessRecorder *ProcessRecorder::instance_ = nullptr;

ProcessRecorder::ProcessRecorder(ProcessCapture *capture, ProcessMemory *memory,
                                 ProcessInput *input)
    : capture_(capture), memory_(memory), input_(input), isRecording_(false), shouldStop_(false),
      currentFrame_(0), droppedFrames_(0), currentLatencyMs_(0.0), maxDurationSeconds_(0),
      keyboardHook_(nullptr), mouseHook_(nullptr), hooksReady_(false) {

    // Initialize stats
    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = 0;
    stats_.endTimeMs = 0;
    stats_.actualDurationSeconds = 0.0;
    stats_.actualFps = 0.0;

    instance_ = this;
}

ProcessRecorder::~ProcessRecorder() {
    if (isRecording_) {
        shouldStop_ = true;
        if (recordingThread_.joinable()) {
            recordingThread_.join();
        }
    }

    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }

    // Stop hook thread
    if (hookThread_.joinable()) {
        PostThreadMessage(GetThreadId(hookThread_.native_handle()), WM_QUIT, 0, 0);
        hookThread_.join();
    }
    
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

std::string ProcessRecorder::GenerateSessionId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::stringstream ss;
    ss << "rec_" << timestamp << "_" << dis(gen);
    return ss.str();
}

bool ProcessRecorder::CreateOutputDirectories() {
    try {
        // Create base output directory if it doesn't exist
        fs::path basePath(outputDirectory_);
        if (!fs::exists(basePath)) {
            fs::create_directories(basePath);
        }

        // Create session-specific subdirectory
        fs::path sessionPath = basePath / sessionId_;
        if (!fs::exists(sessionPath)) {
            fs::create_directories(sessionPath);
        }

        // Create frames subdirectory within session folder
        fs::path framesPath = sessionPath / "frames";
        if (!fs::exists(framesPath)) {
            fs::create_directories(framesPath);
        }

        spdlog::info("Created output directories at: {}", sessionPath.string());
        return true;
    } catch (const std::exception &e) {
        spdlog::error("Failed to create output directories: {}", e.what());
        return false;
    }
}

bool ProcessRecorder::StartRecording(const std::vector<std::string> &attributeNames,
                                     const std::string &outputDirectory, int maxDurationSeconds) {
    if (isRecording_) {
        spdlog::warn("Recording already in progress");
        return false;
    }

    if (!capture_ || !memory_) {
        spdlog::error("Capture or Memory subsystem not initialized");
        return false;
    }

    // Store configuration
    attributeNames_ = attributeNames;
    outputDirectory_ = outputDirectory;
    maxDurationSeconds_ = maxDurationSeconds;
    sessionId_ = GenerateSessionId();

    // Create output directories
    if (!CreateOutputDirectories()) {
        return false;
    }

    // Reset statistics
    currentFrame_ = 0;
    droppedFrames_ = 0;
    currentLatencyMs_ = 0.0;
    frameDataBuffer_.clear();

    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    // Start hook thread (must have message loop for hooks to work)
    hooksReady_ = false;
    hookThread_ = std::thread(&ProcessRecorder::HookMessageLoop, this);

    // Wait for hooks to be ready
    auto startTime = std::chrono::steady_clock::now();
    while (!hooksReady_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > std::chrono::seconds(5)) {
            spdlog::error("Timeout waiting for hooks to initialize");
            if (hookThread_.joinable()) {
                PostThreadMessage(GetThreadId(hookThread_.native_handle()), WM_QUIT, 0, 0);
                hookThread_.join();
            }
            return false;
        }
    }

    // Start recording thread
    shouldStop_ = false;
    isRecording_ = true;

    recordingThread_ = std::thread(&ProcessRecorder::RecordingLoop, this);

    spdlog::info("Recording started - Session ID: {}", sessionId_);
    spdlog::info("Output directory: {}", outputDirectory_);
    spdlog::info("Attributes to record: {}", attributeNames_.size());

    return true;
}

bool ProcessRecorder::StopRecording(RecordingStats &stats) {
    if (!isRecording_) {
        spdlog::warn("No recording in progress");
        return false;
    }

    spdlog::info("Stopping recording...");
    shouldStop_ = true;

    // Wait for recording thread to finish
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }

    isRecording_ = false;

    // Stop hook thread
    if (hookThread_.joinable()) {
        PostThreadMessage(GetThreadId(hookThread_.native_handle()), WM_QUIT, 0, 0);
        hookThread_.join();
    }

    // Write metadata to file
    WriteMetadataToFile();

    // Update final statistics
    stats_.endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    stats_.totalFrames = currentFrame_;
    stats_.droppedFrames = droppedFrames_;

    // Calculate actual duration and FPS
    stats_.actualDurationSeconds = (stats_.endTimeMs - stats_.startTimeMs) / 1000.0;
    stats_.actualFps = stats_.totalFrames / stats_.actualDurationSeconds;

    // Calculate average latency
    if (!frameDataBuffer_.empty()) {
        double totalLatency = 0.0;
        for (const auto &frame : frameDataBuffer_) {
            totalLatency += frame.totalMs;
        }
        stats_.averageLatencyMs = totalLatency / frameDataBuffer_.size();
    }

    stats = stats_;

    spdlog::info("Recording stopped - Total frames: {}, Dropped: {}, Avg latency: {:.2f}ms",
                 stats.totalFrames, stats.droppedFrames, stats.averageLatencyMs);

    return true;
}

bool ProcessRecorder::GetStatus(bool &isRecording, int &currentFrame, double &elapsedTime,
                                double &currentLatency, int &droppedFrames) {
    isRecording = isRecording_;
    currentFrame = currentFrame_;
    droppedFrames = droppedFrames_;
    currentLatency = currentLatencyMs_;

    if (isRecording_) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        elapsedTime = (now - stats_.startTimeMs) / 1000.0;
    } else {
        elapsedTime = 0.0;
    }

    return true;
}

// Static keyboard hook callback
LRESULT CALLBACK ProcessRecorder::KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && instance_) {
        KBDLLHOOKSTRUCT *pKeyBoard = (KBDLLHOOKSTRUCT *)lParam;
        std::string keyName = instance_->VirtualKeyToString(pKeyBoard->vkCode);

        std::lock_guard<std::mutex> lock(instance_->keystrokeMutex_);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // Key pressed
            if (std::find(instance_->currentKeysPressed_.begin(),
                          instance_->currentKeysPressed_.end(),
                          keyName) == instance_->currentKeysPressed_.end()) {
                instance_->currentKeysPressed_.push_back(keyName);
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            // Key released
            instance_->currentKeysPressed_.erase(std::remove(instance_->currentKeysPressed_.begin(),
                                                             instance_->currentKeysPressed_.end(),
                                                             keyName),
                                                 instance_->currentKeysPressed_.end());
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Convert Windows Virtual Key Code to string
std::string ProcessRecorder::VirtualKeyToString(DWORD vkCode) {
    // Map virtual key codes to key names (matching Interception naming)
    static std::map<DWORD, std::string> vkMap = {{VK_ESCAPE, "ESC"},
                                                 {VK_BACK, "BACKSPACE"},
                                                 {VK_TAB, "TAB"},
                                                 {VK_RETURN, "ENTER"},
                                                 {VK_SPACE, "SPACE"},
                                                 {VK_CAPITAL, "CAPSLOCK"},
                                                 {VK_NUMLOCK, "NUMLOCK"},
                                                 {VK_SCROLL, "SCROLLLOCK"},
                                                 {VK_LSHIFT, "LEFT_SHIFT"},
                                                 {VK_RSHIFT, "RIGHT_SHIFT"},
                                                 {VK_LCONTROL, "LEFT_CTRL"},
                                                 {VK_LMENU, "LEFT_ALT"},

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

                                                 // Letters
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

                                                 // Numbers
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
                                                 {VK_OEM_2, "SLASH"}};

    auto it = vkMap.find(vkCode);
    if (it != vkMap.end()) {
        return it->second;
    }

    return "UNKNOWN_" + std::to_string(vkCode);
}

// Static mouse hook callback
LRESULT CALLBACK ProcessRecorder::MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && instance_) {
        std::lock_guard<std::mutex> lock(instance_->keystrokeMutex_);

        std::string buttonName;
        bool isPress = false;

        switch (wParam) {
        case WM_LBUTTONDOWN:
            buttonName = "MOUSE_LEFT";
            isPress = true;
            break;
        case WM_LBUTTONUP:
            buttonName = "MOUSE_LEFT";
            isPress = false;
            break;
        case WM_RBUTTONDOWN:
            buttonName = "MOUSE_RIGHT";
            isPress = true;
            break;
        case WM_RBUTTONUP:
            buttonName = "MOUSE_RIGHT";
            isPress = false;
            break;
        case WM_MBUTTONDOWN:
            buttonName = "MOUSE_MIDDLE";
            isPress = true;
            break;
        case WM_MBUTTONUP:
            buttonName = "MOUSE_MIDDLE";
            isPress = false;
            break;
        case WM_XBUTTONDOWN: {
            MSLLHOOKSTRUCT *pMouse = (MSLLHOOKSTRUCT *)lParam;
            if (HIWORD(pMouse->mouseData) == XBUTTON1) {
                buttonName = "MOUSE_BUTTON4";
            } else if (HIWORD(pMouse->mouseData) == XBUTTON2) {
                buttonName = "MOUSE_BUTTON5";
            }
            isPress = true;
            break;
        }
        case WM_XBUTTONUP: {
            MSLLHOOKSTRUCT *pMouse = (MSLLHOOKSTRUCT *)lParam;
            if (HIWORD(pMouse->mouseData) == XBUTTON1) {
                buttonName = "MOUSE_BUTTON4";
            } else if (HIWORD(pMouse->mouseData) == XBUTTON2) {
                buttonName = "MOUSE_BUTTON5";
            }
            isPress = false;
            break;
        }
        }

        if (!buttonName.empty()) {
            if (isPress) {
                // Button pressed
                if (std::find(instance_->currentKeysPressed_.begin(),
                             instance_->currentKeysPressed_.end(),
                             buttonName) == instance_->currentKeysPressed_.end()) {
                    instance_->currentKeysPressed_.push_back(buttonName);
                }
            } else {
                // Button released
                instance_->currentKeysPressed_.erase(
                    std::remove(instance_->currentKeysPressed_.begin(),
                               instance_->currentKeysPressed_.end(), buttonName),
                    instance_->currentKeysPressed_.end());
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

std::vector<std::string> ProcessRecorder::GetCurrentKeysPressed() {
    std::lock_guard<std::mutex> lock(keystrokeMutex_);
    return currentKeysPressed_;
}

// Hook message loop thread - required for hooks to work
void ProcessRecorder::HookMessageLoop() {
    spdlog::info("Hook message loop thread started");

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

    spdlog::info("Keyboard and mouse hooks installed successfully");
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

    spdlog::info("Hook message loop thread stopped");
}

void ProcessRecorder::RecordingLoop() {
    spdlog::info("Recording loop started - Target: 60fps (16.67ms per frame)");

    const auto targetFrameTime = std::chrono::microseconds(16667); // 60fps = 16.67ms
    auto lastFrameTime = std::chrono::high_resolution_clock::now();

    while (!shouldStop_) {
        auto frameStartTime = std::chrono::high_resolution_clock::now();
        auto wallClockTime = std::chrono::system_clock::now();

        // Check max duration
        if (maxDurationSeconds_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               frameStartTime - std::chrono::high_resolution_clock::time_point(
                                                    std::chrono::milliseconds(stats_.startTimeMs)))
                               .count();
            if (elapsed >= maxDurationSeconds_) {
                spdlog::info("Max duration reached, stopping recording");
                break;
            }
        }

        // Process Windows messages for keyboard hook to work
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Check if a new frame is available from Windows Graphics Capture
        if (!capture_->IsNewFrameAvailable()) {
            // No new frame yet, sleep briefly and continue
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        FrameData frameData;
        frameData.frameNumber = currentFrame_;
        frameData.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(wallClockTime.time_since_epoch())
                .count();

        // Capture frame
        auto captureStart = std::chrono::high_resolution_clock::now();
        std::vector<uint8_t> pixels = capture_->GetPixelData();
        auto captureEnd = std::chrono::high_resolution_clock::now();
        frameData.frameCaptureMs =
            std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();

        // Read memory attributes
        auto memoryStart = std::chrono::high_resolution_clock::now();
        ReadMemoryAttributes(frameData);
        auto memoryEnd = std::chrono::high_resolution_clock::now();
        frameData.memoryReadMs =
            std::chrono::duration<double, std::milli>(memoryEnd - memoryStart).count();

        // Capture keystrokes
        auto keystrokeStart = std::chrono::high_resolution_clock::now();
        CaptureKeystrokes(frameData);
        auto keystrokeEnd = std::chrono::high_resolution_clock::now();
        frameData.keystrokeCaptureMs =
            std::chrono::duration<double, std::milli>(keystrokeEnd - keystrokeStart).count();

        // Write frame to disk
        auto diskStart = std::chrono::high_resolution_clock::now();
        WriteFrameToDisk(pixels, frameData.frameNumber);
        auto diskEnd = std::chrono::high_resolution_clock::now();
        frameData.diskWriteMs =
            std::chrono::duration<double, std::milli>(diskEnd - diskStart).count();

        auto frameEndTime = std::chrono::high_resolution_clock::now();
        frameData.totalMs =
            std::chrono::duration<double, std::milli>(frameEndTime - frameStartTime).count();

        // Update statistics
        currentLatencyMs_ = frameData.totalMs;
        if (frameData.totalMs > stats_.maxLatencyMs) {
            stats_.maxLatencyMs = frameData.totalMs;
        }
        if (frameData.totalMs < stats_.minLatencyMs) {
            stats_.minLatencyMs = frameData.totalMs;
        }

        // Store frame data for metadata
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            frameDataBuffer_.push_back(frameData);
        }

        // Check if we exceeded frame time budget
        if (frameData.totalMs > 16.67) {
            droppedFrames_++;
            spdlog::warn("Frame {} exceeded time budget: {:.2f}ms", frameData.frameNumber,
                         frameData.totalMs);
        }

        currentFrame_++;

        // Log progress every 60 frames
        if (currentFrame_ % 60 == 0 && currentFrame_ > 0) {
            auto currentTime = std::chrono::system_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               currentTime.time_since_epoch())
                               .count() -
                           stats_.startTimeMs;
            double actualFps = (currentFrame_ * 1000.0) / elapsed;
            spdlog::info("Frame {}: {:.2f}ms (capture: {:.2f}ms, memory: {:.2f}ms, keys: "
                         "{:.2f}ms, disk: {:.2f}ms) - FPS: {:.1f}",
                         frameData.frameNumber, frameData.totalMs, frameData.frameCaptureMs,
                         frameData.memoryReadMs, frameData.keystrokeCaptureMs,
                         frameData.diskWriteMs, actualFps);
        }

        lastFrameTime = frameEndTime;
    }

    spdlog::info("Recording loop stopped");
}

bool ProcessRecorder::ReadMemoryAttributes(FrameData &frameData) {
    if (!memory_) {
        return false;
    }

    for (const auto &attrName : attributeNames_) {
        try {
            ProcessAttribute attr = memory_->GetAttribute(attrName);

            if (attr.AttributeType == "int") {
                int32_t value = 0;
                if (memory_->ExtractAttributeInt(attrName, value)) {
                    frameData.memoryData[attrName] = std::to_string(value);
                }
            } else if (attr.AttributeType == "float") {
                float value = 0.0f;
                if (memory_->ExtractAttributeFloat(attrName, value)) {
                    frameData.memoryData[attrName] = std::to_string(value);
                }
            } else if (attr.AttributeType == "array") {
                std::vector<uint8_t> value(attr.AttributeLength);
                if (memory_->ExtractAttributeArray(attrName, value)) {
                    // Convert array to hex string
                    std::stringstream ss;
                    ss << "[";
                    for (size_t i = 0; i < value.size(); ++i) {
                        if (i > 0)
                            ss << ",";
                        ss << static_cast<int>(value[i]);
                    }
                    ss << "]";
                    frameData.memoryData[attrName] = ss.str();
                }
            } else if (attr.AttributeType == "bool") {
                std::vector<uint8_t> value(1);
                if (memory_->ExtractAttributeArray(attrName, value)) {
                    frameData.memoryData[attrName] = value[0] ? "true" : "false";
                }
            }
        } catch (const std::exception &e) {
            spdlog::error("Failed to read attribute {}: {}", attrName, e.what());
        }
    }

    return true;
}

bool ProcessRecorder::CaptureKeystrokes(FrameData &frameData) {
    frameData.keysPressed = GetCurrentKeysPressed();
    return true;
}

bool ProcessRecorder::WriteFrameToDisk(const std::vector<uint8_t> &pixels, int frameNumber) {
    if (pixels.empty()) {
        spdlog::error("Cannot write empty frame {}", frameNumber);
        return false;
    }

    try {
        // Build path: outputDirectory/sessionId/frames/frame_NNNNNN.bmp
        fs::path framesPath = fs::path(outputDirectory_) / sessionId_ / "frames";

        std::stringstream filename;
        filename << "frame_" << std::setw(6) << std::setfill('0') << frameNumber << ".bmp";
        fs::path framePath = framesPath / filename.str();

        // Use ProcessCapture's SaveBMP function
        return capture_->SaveBMP(pixels, framePath.string().c_str());

    } catch (const std::exception &e) {
        spdlog::error("Failed to write frame {} to disk: {}", frameNumber, e.what());
        return false;
    }
}

bool ProcessRecorder::WriteMetadataToFile() {
    try {
        // Build path: outputDirectory/sessionId/metadata.json
        fs::path metadataPath = fs::path(outputDirectory_) / sessionId_ / "metadata.json";
        std::ofstream outFile(metadataPath);

        if (!outFile.is_open()) {
            spdlog::error("Failed to open metadata file for writing");
            return false;
        }

        outFile << "{\n";
        outFile << "  \"session_id\": \"" << sessionId_ << "\",\n";
        outFile << "  \"start_time_ms\": " << stats_.startTimeMs << ",\n";
        outFile << "  \"end_time_ms\": " << stats_.endTimeMs << ",\n";
        outFile << "  \"total_frames\": " << stats_.totalFrames << ",\n";
        outFile << "  \"dropped_frames\": " << stats_.droppedFrames << ",\n";
        outFile << "  \"actual_duration_seconds\": " << stats_.actualDurationSeconds << ",\n";
        outFile << "  \"actual_fps\": " << stats_.actualFps << ",\n";
        outFile << "  \"average_latency_ms\": " << stats_.averageLatencyMs << ",\n";
        outFile << "  \"max_latency_ms\": " << stats_.maxLatencyMs << ",\n";
        outFile << "  \"min_latency_ms\": " << stats_.minLatencyMs << ",\n";
        outFile << "  \"frames\": [\n";

        std::lock_guard<std::mutex> lock(bufferMutex_);
        for (size_t i = 0; i < frameDataBuffer_.size(); ++i) {
            const auto &frame = frameDataBuffer_[i];

            outFile << "    {\n";
            outFile << "      \"frame\": " << frame.frameNumber << ",\n";
            outFile << "      \"timestamp_ms\": " << frame.timestampMs << ",\n";

            // Memory data
            outFile << "      \"memory_data\": {\n";
            size_t memIdx = 0;
            for (const auto &pair : frame.memoryData) {
                outFile << "        \"" << pair.first << "\": \"" << pair.second << "\"";
                if (memIdx < frame.memoryData.size() - 1)
                    outFile << ",";
                outFile << "\n";
                memIdx++;
            }
            outFile << "      },\n";

            // Keys pressed
            outFile << "      \"keys_pressed\": [";
            for (size_t j = 0; j < frame.keysPressed.size(); ++j) {
                outFile << "\"" << frame.keysPressed[j] << "\"";
                if (j < frame.keysPressed.size() - 1)
                    outFile << ", ";
            }
            outFile << "],\n";

            // Latency
            outFile << "      \"latency_ms\": {\n";
            outFile << "        \"frame_capture\": " << frame.frameCaptureMs << ",\n";
            outFile << "        \"memory_read\": " << frame.memoryReadMs << ",\n";
            outFile << "        \"keystroke_capture\": " << frame.keystrokeCaptureMs << ",\n";
            outFile << "        \"disk_write\": " << frame.diskWriteMs << ",\n";
            outFile << "        \"total\": " << frame.totalMs << "\n";
            outFile << "      }\n";

            outFile << "    }";
            if (i < frameDataBuffer_.size() - 1)
                outFile << ",";
            outFile << "\n";
        }

        outFile << "  ]\n";
        outFile << "}\n";

        outFile.close();

        spdlog::info("Metadata written to: {}", metadataPath.string());
        return true;

    } catch (const std::exception &e) {
        spdlog::error("Failed to write metadata file: {}", e.what());
        return false;
    }
}
