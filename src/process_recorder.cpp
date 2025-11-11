#include "process_recorder.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <spdlog/spdlog.h>
#include <sstream>

namespace fs = std::filesystem;

ProcessRecorder::ProcessRecorder(ProcessCapture *capture, ProcessMemory *memory,
                                 ProcessInput *input)
    : capture_(capture), memory_(memory), input_(input), isRecording_(false), shouldStop_(false),
      currentFrame_(0), droppedFrames_(0), currentLatencyMs_(0.0), maxDurationSeconds_(0),
      captureWidth_(0), captureHeight_(0), targetWindow_(nullptr) {

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

    // Create input event logger
    inputLogger_ = std::make_unique<InputEventLogger>();

    // Create video encoder
    videoEncoder_ = std::make_unique<VideoEncoder>();
}

ProcessRecorder::~ProcessRecorder() {
    if (isRecording_) {
        shouldStop_ = true;
        if (recordingThread_.joinable()) {
            recordingThread_.join();
        }
    }

    // Stop input logger if running
    if (inputLogger_ && inputLogger_->IsLogging()) {
        inputLogger_->StopLogging();
    }

    // Stop video encoder if running
    if (videoEncoder_) {
        videoEncoder_->Finalize();
    }

    // Close memory file if open
    if (memoryFile_.is_open()) {
        memoryFile_.close();
    }

    // Cleanup DXGI resources
    CleanupDXGICapture();
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

    stats_.totalFrames = 0;
    stats_.droppedFrames = 0;
    stats_.averageLatencyMs = 0.0;
    stats_.maxLatencyMs = 0.0;
    stats_.minLatencyMs = 999999.0;
    stats_.startTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    // Initialize DXGI Desktop Duplication for recording
    if (!InitializeDXGICapture(capture_->processWindow)) {
        spdlog::error("Failed to initialize DXGI capture, falling back to WGC");
        // Continue with WGC - don't fail completely
    }

    // Initialize video encoder (lossless FFV1)
    try {
        std::string videoPath = (fs::path(outputDirectory_) / sessionId_ / "video.mp4").string();
        int width = dxgiDuplication_ ? captureWidth_ : capture_->processWindowWidth;
        int height = dxgiDuplication_ ? captureHeight_ : capture_->processWindowHeight;

        if (!videoEncoder_->Initialize(videoPath, width, height, 60)) {
            spdlog::error("Failed to initialize video encoder");
            return false;
        }
        spdlog::info("Initialized video encoder (FFV1 lossless): {}", videoPath);
    } catch (const std::exception &e) {
        spdlog::error("Failed to initialize video encoder: {}", e.what());
        return false;
    }

    // Initialize memory data CSV file
    if (!attributeNames_.empty()) {
        std::string memoryPath =
            (fs::path(outputDirectory_) / sessionId_ / "memory_data.csv").string();
        memoryFile_.open(memoryPath, std::ios::out | std::ios::trunc);
        if (!memoryFile_.is_open()) {
            spdlog::error("Failed to open memory data file: {}", memoryPath);
            return false;
        }
        WriteMemoryHeader();
        spdlog::info("Initialized memory data CSV: {}", memoryPath);
    }

    // Initialize performance data CSV file
    std::string perfPath = (fs::path(outputDirectory_) / sessionId_ / "perf_data.csv").string();
    perfFile_.open(perfPath, std::ios::out | std::ios::trunc);
    if (!perfFile_.is_open()) {
        spdlog::error("Failed to open perf data file: {}", perfPath);
        return false;
    }
    WritePerfHeader();
    spdlog::info("Initialized perf data CSV: {}", perfPath);

    // Start input event logger (independent of video recording)
    std::string inputLogPath = (fs::path(outputDirectory_) / sessionId_ / "inputs.csv").string();
    if (!inputLogger_->StartLogging(inputLogPath)) {
        spdlog::error("Failed to start input event logger");
        return false;
    }

    // Start recording threads
    shouldStop_ = false;
    isRecording_ = true;

    recordingThread_ = std::thread(&ProcessRecorder::RecordingLoop, this);

    // Start memory reading thread (runs independently)
    if (!attributeNames_.empty()) {
        memoryThread_ = std::thread(&ProcessRecorder::MemoryReadingLoop, this);
    }

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

    // Wait for recording threads to finish
    if (recordingThread_.joinable()) {
        recordingThread_.join();
    }
    if (memoryThread_.joinable()) {
        memoryThread_.join();
    }

    isRecording_ = false;

    // Stop input logger
    if (inputLogger_ && inputLogger_->IsLogging()) {
        inputLogger_->StopLogging();
    }

    // Finalize video encoder (waits for queue to drain)
    if (videoEncoder_) {
        spdlog::info("Finalizing video encoder - queue size: {}", videoEncoder_->GetQueueSize());
        videoEncoder_->Finalize();
        spdlog::info("Video finalized - frames encoded: {}", videoEncoder_->GetFramesEncoded());
    }

    // Close memory file
    if (memoryFile_.is_open()) {
        memoryFile_.close();
    }

    // Close perf file
    if (perfFile_.is_open()) {
        perfFile_.close();
    }

    // Cleanup DXGI capture
    CleanupDXGICapture();

    // Update final statistics
    stats_.endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    stats_.totalFrames = currentFrame_;
    stats_.droppedFrames = droppedFrames_;

    // Calculate actual duration and FPS
    stats_.actualDurationSeconds = (stats_.endTimeMs - stats_.startTimeMs) / 1000.0;
    stats_.actualFps = stats_.totalFrames / stats_.actualDurationSeconds;

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

void ProcessRecorder::WriteMemoryHeader() {
    if (!memoryFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(memoryMutex_);
    memoryFile_ << "timestamp_us";
    for (const auto &attrName : attributeNames_) {
        memoryFile_ << "," << attrName;
    }
    memoryFile_ << "\n";
    memoryFile_.flush();
}

void ProcessRecorder::WriteMemoryFrame(const MemoryFrameData &data) {
    if (!memoryFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(memoryMutex_);
    memoryFile_ << data.timestampUs;
    for (const auto &attrName : attributeNames_) {
        auto it = data.memoryData.find(attrName);
        if (it != data.memoryData.end()) {
            memoryFile_ << "," << it->second;
        } else {
            memoryFile_ << ",0";
        }
    }
    memoryFile_ << "\n";

    // Flush every 60 frames
    if (currentFrame_ % 60 == 0) {
        memoryFile_.flush();
    }
}

void ProcessRecorder::WritePerfHeader() {
    if (!perfFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(perfMutex_);
    perfFile_ << "frame,timestamp_us,total_ms,capture_ms,fps,queue_size,dropped_frames\n";
    perfFile_.flush();
}

void ProcessRecorder::WritePerfData(int frame, int64_t timestampUs, double totalMs,
                                    double captureMs, double fps, size_t queueSize, int dropped) {
    if (!perfFile_.is_open())
        return;

    std::lock_guard<std::mutex> lock(perfMutex_);
    perfFile_ << frame << "," << timestampUs << "," << totalMs << "," << captureMs << "," << fps
              << "," << queueSize << "," << dropped << "\n";

    // Flush every 60 frames
    if (frame % 60 == 0) {
        perfFile_.flush();
    }
}

void ProcessRecorder::RecordingLoop() {
    if (dxgiDuplication_) {
        spdlog::info("==> Using DXGI Desktop Duplication (polling mode)");
    } else {
        spdlog::info("==> Using Windows Graphics Capture (event-based fallback)");
    }
    spdlog::info("Recording loop started - Capturing at game framerate");

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

        int64_t timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(wallClockTime.time_since_epoch())
                .count();

        // Capture video frame using DXGI if available, otherwise fall back to WGC
        auto captureStart = std::chrono::high_resolution_clock::now();
        std::vector<uint8_t> pixels;

        if (dxgiDuplication_) {
            // Use DXGI Desktop Duplication (60fps polling)
            pixels = CaptureFrameDXGI();
            // If no new frame, keep using last frame
            static std::vector<uint8_t> lastFrame;
            if (!pixels.empty()) {
                lastFrame = pixels;
            } else if (!lastFrame.empty()) {
                pixels = lastFrame;
            }
        } else {
            // Fall back to WGC
            pixels = capture_->GetPixelData();
        }

        auto captureEnd = std::chrono::high_resolution_clock::now();
        double frameCaptureMs =
            std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();

        // Queue frame for video encoding
        EncoderFrame videoFrame;
        videoFrame.pixels = std::move(pixels);
        videoFrame.timestampUs = timestampUs;
        videoFrame.width = dxgiDuplication_ ? captureWidth_ : capture_->processWindowWidth;
        videoFrame.height = dxgiDuplication_ ? captureHeight_ : capture_->processWindowHeight;
        videoEncoder_->EncodeFrame(std::move(videoFrame));

        // Calculate total frame time
        auto frameEndTime = std::chrono::high_resolution_clock::now();
        double totalMs =
            std::chrono::duration<double, std::milli>(frameEndTime - frameStartTime).count();

        // Update statistics
        currentLatencyMs_ = totalMs;
        if (totalMs > stats_.maxLatencyMs) {
            stats_.maxLatencyMs = totalMs;
        }
        if (totalMs < stats_.minLatencyMs) {
            stats_.minLatencyMs = totalMs;
        }

        // Accumulate average latency
        stats_.averageLatencyMs =
            (stats_.averageLatencyMs * currentFrame_ + totalMs) / (currentFrame_ + 1);

        // Check if we exceeded frame time budget (60fps = 16.67ms)
        if (totalMs > 16.67) {
            droppedFrames_++;
        }

        currentFrame_++;

        // Write performance data to CSV
        auto currentTime = std::chrono::system_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime.time_since_epoch())
                .count() -
            stats_.startTimeMs;
        double actualFps = elapsed > 0 ? (currentFrame_ * 1000.0) / elapsed : 0.0;
        size_t videoQueueSize = videoEncoder_->GetQueueSize();

        WritePerfData(currentFrame_, timestampUs, totalMs, frameCaptureMs, actualFps,
                      videoQueueSize, droppedFrames_);

        lastFrameTime = frameEndTime;
    }

    spdlog::info("Recording loop stopped");
}

void ProcessRecorder::MemoryReadingLoop() {
    spdlog::info("Memory reading thread started");

    while (!shouldStop_) {
        auto wallClockTime = std::chrono::system_clock::now();
        int64_t timestampUs =
            std::chrono::duration_cast<std::chrono::microseconds>(wallClockTime.time_since_epoch())
                .count();

        // Read memory attributes
        MemoryFrameData memoryData;
        memoryData.timestampUs = timestampUs;

        for (const auto &attrName : attributeNames_) {
            try {
                ProcessAttribute attr = memory_->GetAttribute(attrName);
                std::string value;

                if (attr.AttributeType == "int") {
                    int32_t intVal = 0;
                    if (memory_->ExtractAttributeInt(attrName, intVal)) {
                        value = std::to_string(intVal);
                    }
                } else if (attr.AttributeType == "float") {
                    float floatVal = 0.0f;
                    if (memory_->ExtractAttributeFloat(attrName, floatVal)) {
                        value = std::to_string(floatVal);
                    }
                } else {
                    value = "0";
                }

                memoryData.memoryData[attrName] = value;
            } catch (const std::exception &e) {
                memoryData.memoryData[attrName] = "0";
            }
        }

        // Write memory data to CSV
        WriteMemoryFrame(memoryData);

        // Sleep for a bit to avoid hammering the process memory
        // Aiming for ~30 Hz memory sampling (independent of video capture)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    spdlog::info("Memory reading thread stopped");
}

bool ProcessRecorder::InitializeDXGICapture(HWND window) {
    spdlog::info("Initializing DXGI Desktop Duplication capture...");

    targetWindow_ = window;

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dContext_);

    if (FAILED(hr)) {
        spdlog::error("Failed to create D3D11 device: 0x{:X}", hr);
        return false;
    }

    // Get DXGI device
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice_.As(&dxgiDevice);
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI device: 0x{:X}", hr);
        return false;
    }

    // Get DXGI adapter
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) {
        spdlog::error("Failed to get DXGI adapter: 0x{:X}", hr);
        return false;
    }

    // Get monitor that contains the window
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);

    // Find the output (monitor) that matches
    ComPtr<IDXGIOutput> dxgiOutput;
    UINT outputIndex = 0;
    while (dxgiAdapter->EnumOutputs(outputIndex, &dxgiOutput) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC outputDesc;
        dxgiOutput->GetDesc(&outputDesc);

        if (outputDesc.Monitor == monitor) {
            break;
        }

        dxgiOutput = nullptr;
        outputIndex++;
    }

    if (!dxgiOutput) {
        spdlog::error("Failed to find DXGI output for window's monitor");
        return false;
    }

    // Get IDXGIOutput1
    ComPtr<IDXGIOutput1> dxgiOutput1;
    hr = dxgiOutput.As(&dxgiOutput1);
    if (FAILED(hr)) {
        spdlog::error("Failed to get IDXGIOutput1: 0x{:X}", hr);
        return false;
    }

    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(d3dDevice_.Get(), &dxgiDuplication_);
    if (FAILED(hr)) {
        spdlog::error("Failed to create desktop duplication: 0x{:X}", hr);
        spdlog::error("Make sure no other desktop duplication is active");
        return false;
    }

    // Get duplication description for dimensions
    DXGI_OUTDUPL_DESC duplDesc;
    dxgiDuplication_->GetDesc(&duplDesc);

    // Get window size to determine capture area
    RECT windowRect;
    GetWindowRect(window, &windowRect);
    captureWidth_ = windowRect.right - windowRect.left;
    captureHeight_ = windowRect.bottom - windowRect.top;

    spdlog::info("DXGI Capture initialized: {}x{}", captureWidth_, captureHeight_);
    spdlog::info("Desktop size: {}x{}", duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height);

    // Create staging texture for CPU readback
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = captureWidth_;
    stagingDesc.Height = captureHeight_;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_);
    if (FAILED(hr)) {
        spdlog::error("Failed to create staging texture: 0x{:X}", hr);
        return false;
    }

    spdlog::info("DXGI Desktop Duplication ready for 60fps capture");
    return true;
}

void ProcessRecorder::CleanupDXGICapture() {
    if (dxgiDuplication_) {
        dxgiDuplication_->ReleaseFrame();
        dxgiDuplication_ = nullptr;
    }

    stagingTexture_ = nullptr;
    d3dContext_ = nullptr;
    d3dDevice_ = nullptr;

    spdlog::info("DXGI capture cleanup complete");
}

std::vector<uint8_t> ProcessRecorder::CaptureFrameDXGI() {
    std::vector<uint8_t> pixels;

    if (!dxgiDuplication_) {
        return pixels;
    }

    // Try to acquire next frame (with timeout)
    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = dxgiDuplication_->AcquireNextFrame(0, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame, return empty (we'll use last frame)
        return pixels;
    }

    if (FAILED(hr)) {
        spdlog::warn("AcquireNextFrame failed: 0x{:X}", hr);
        return pixels;
    }

    // Get texture from resource
    ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr)) {
        dxgiDuplication_->ReleaseFrame();
        return pixels;
    }

    // Get window position on screen
    RECT windowRect;
    GetWindowRect(targetWindow_, &windowRect);

    // Copy window region to staging texture
    D3D11_BOX sourceRegion;
    sourceRegion.left = windowRect.left;
    sourceRegion.right = windowRect.right;
    sourceRegion.top = windowRect.top;
    sourceRegion.bottom = windowRect.bottom;
    sourceRegion.front = 0;
    sourceRegion.back = 1;

    d3dContext_->CopySubresourceRegion(stagingTexture_.Get(), 0, 0, 0, 0, desktopTexture.Get(), 0,
                                       &sourceRegion);

    // Map staging texture to read pixels
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = d3dContext_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        // Copy pixel data
        pixels.resize(captureWidth_ * captureHeight_ * 4);

        uint8_t *src = static_cast<uint8_t *>(mapped.pData);
        uint8_t *dst = pixels.data();

        for (int y = 0; y < captureHeight_; y++) {
            memcpy(dst + y * captureWidth_ * 4, src + y * mapped.RowPitch, captureWidth_ * 4);
        }

        d3dContext_->Unmap(stagingTexture_.Get(), 0);
    }

    // Release the frame
    dxgiDuplication_->ReleaseFrame();

    return pixels;
}
