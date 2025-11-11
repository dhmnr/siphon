#pragma once

#include "input_event_logger.h"
#include "interception.h"
#include "process_attribute.h"
#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include "video_encoder.h"
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Structure for memory data per frame
struct MemoryFrameData {
    int64_t timestampUs;
    std::map<std::string, std::string> memoryData;
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
    std::thread memoryThread_;

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

    // Video encoder (lossless)
    std::unique_ptr<VideoEncoder> videoEncoder_;

    // Input event logger (runs independently)
    std::unique_ptr<InputEventLogger> inputLogger_;

    // Memory data CSV writer
    std::ofstream memoryFile_;
    std::mutex memoryMutex_;

    // Performance data CSV writer
    std::ofstream perfFile_;
    std::mutex perfMutex_;

    // DXGI Desktop Duplication for recording
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGIOutputDuplication> dxgiDuplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    int captureWidth_;
    int captureHeight_;
    HWND targetWindow_;

    // Private methods
    void RecordingLoop();
    void MemoryReadingLoop();
    void WriteMemoryHeader();
    void WriteMemoryFrame(const MemoryFrameData &data);
    void WritePerfHeader();
    void WritePerfData(int frame, int64_t timestampUs, double totalMs, double captureMs, double fps,
                       size_t queueSize, int dropped);
    std::string GenerateSessionId();
    bool CreateOutputDirectories();
    bool InitializeDXGICapture(HWND window);
    void CleanupDXGICapture();
    std::vector<uint8_t> CaptureFrameDXGI();

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
