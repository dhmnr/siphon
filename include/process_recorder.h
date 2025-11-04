#pragma once

#include "h5_recording_writer.h"
#include "interception.h"
#include "process_attribute.h"
#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// Structure to hold per-frame recording data
struct FrameData {
    int frameNumber;
    int64_t timestampMs;
    std::map<std::string, std::string> memoryData; // attribute name -> value as string
    std::vector<std::string> keysPressed;

    // Latency measurements in milliseconds
    double frameCaptureMs;
    double memoryReadMs;
    double keystrokeCaptureMs;
    double diskWriteMs;
    double totalMs;
};

// Recording session statistics
struct RecordingStats {
    int totalFrames;
    int droppedFrames;
    double averageLatencyMs;
    double maxLatencyMs;
    double minLatencyMs;
    int64_t startTimeMs;
    int64_t endTimeMs;
    double actualDurationSeconds;
    double actualFps;
};

class ProcessRecorder {
  private:
    // Dependencies
    ProcessCapture *capture_;
    ProcessMemory *memory_;
    ProcessInput *input_;

    // Recording state
    std::atomic<bool> isRecording_;
    std::atomic<bool> shouldStop_;
    std::thread recordingThread_;
    std::thread hookThread_;

    // Configuration
    std::vector<std::string> attributeNames_;
    std::string outputDirectory_;
    std::string sessionId_;
    int maxDurationSeconds_;

    // Statistics
    std::atomic<int> currentFrame_;
    std::atomic<int> droppedFrames_;
    std::atomic<double> currentLatencyMs_;
    RecordingStats stats_;
    std::mutex statsMutex_;

    // Keystroke and mouse monitoring
    std::vector<std::string> currentKeysPressed_;
    std::mutex keystrokeMutex_;
    HHOOK keyboardHook_;
    HHOOK mouseHook_;
    std::atomic<bool> hooksReady_;
    static ProcessRecorder *instance_; // For static hook callback

    // HDF5 async writer
    std::unique_ptr<H5RecordingWriter> h5Writer_;

    // Private methods
    void RecordingLoop();
    void HookMessageLoop();
    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    std::string GenerateSessionId();
    bool CreateOutputDirectories();
    std::vector<std::string> GetCurrentKeysPressed();
    std::string VirtualKeyToString(DWORD vkCode);

  public:
    ProcessRecorder(ProcessCapture *capture, ProcessMemory *memory, ProcessInput *input);
    ~ProcessRecorder();

    // Main API
    bool StartRecording(const std::vector<std::string> &attributeNames,
                        const std::string &outputDirectory, int maxDurationSeconds);
    bool StopRecording(RecordingStats &stats);
    bool GetStatus(bool &isRecording, int &currentFrame, double &elapsedTime,
                   double &currentLatency, int &droppedFrames);

    std::string GetSessionId() const { return sessionId_; }
};
