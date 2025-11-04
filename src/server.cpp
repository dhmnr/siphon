#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "process_attribute.h"
#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include "process_recorder.h"
#include "siphon_service.grpc.pb.h"
#include "utils.h"
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using grpc::StatusCode;
using siphon_service::CaptureFrameRequest;
using siphon_service::CaptureFrameResponse;
using siphon_service::DownloadRecordingRequest;
using siphon_service::ExecuteCommandRequest;
using siphon_service::ExecuteCommandResponse;
using siphon_service::GetRecordingStatusRequest;
using siphon_service::GetRecordingStatusResponse;
using siphon_service::GetServerStatusRequest;
using siphon_service::GetServerStatusResponse;
using siphon_service::GetSiphonRequest;
using siphon_service::GetSiphonResponse;
using siphon_service::InitializeCaptureRequest;
using siphon_service::InitializeCaptureResponse;
using siphon_service::InitializeInputRequest;
using siphon_service::InitializeInputResponse;
using siphon_service::InitializeMemoryRequest;
using siphon_service::InitializeMemoryResponse;
using siphon_service::InputKeyTapRequest;
using siphon_service::InputKeyTapResponse;
using siphon_service::InputKeyToggleRequest;
using siphon_service::InputKeyToggleResponse;
using siphon_service::MoveMouseRequest;
using siphon_service::MoveMouseResponse;
using siphon_service::ProcessAttributeProto;
using siphon_service::RecordingChunk;
using siphon_service::SetProcessConfigRequest;
using siphon_service::SetProcessConfigResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;
using siphon_service::StartRecordingRequest;
using siphon_service::StartRecordingResponse;
using siphon_service::StopRecordingRequest;
using siphon_service::StopRecordingResponse;

class SiphonServiceImpl final : public SiphonService::Service {
  private:
    std::unique_ptr<ProcessMemory> memory_;
    std::unique_ptr<ProcessInput> input_;
    std::unique_ptr<ProcessCapture> capture_;
    std::unique_ptr<ProcessRecorder> recorder_;
    mutable std::mutex mutex_;

    // Configuration storage
    std::string processName_;
    std::string processWindowName_;
    std::map<std::string, ProcessAttribute> processAttributes_;
    HWND processWindow_ = nullptr;
    DWORD processId_ = 0;
    bool configSet_ = false;

  public:
    SiphonServiceImpl()
        : memory_(nullptr), input_(nullptr), capture_(nullptr), recorder_(nullptr) {}

    Status GetAttribute(ServerContext *context, const GetSiphonRequest *request,
                        GetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (memory_ == nullptr) { // Changed from != 0
            spdlog::error("Memory not initialized or {} address not found",
                          request->attributename());
            response->set_success(false);
            response->set_message("Memory not initialized or " + request->attributename() +
                                  " address not found");
            return Status::OK;
        }

        ProcessAttribute attribute = memory_->GetAttribute(request->attributename());
        bool success = false;
        std::string attributeValueStr; // For logging

        if (attribute.AttributeType == "int") {
            int32_t attributeValue = 0;
            success = memory_->ExtractAttributeInt(request->attributename(), attributeValue);
            if (success) {
                response->set_int_value(attributeValue);
                attributeValueStr = std::to_string(attributeValue);
            }
        } else if (attribute.AttributeType == "float") {
            float attributeValue = 0;
            success = memory_->ExtractAttributeFloat(request->attributename(), attributeValue);
            if (success) {
                response->set_float_value(attributeValue);
                attributeValueStr = std::to_string(attributeValue);
            }
        } else if (attribute.AttributeType == "array") {
            std::vector<uint8_t> attributeValue(attribute.AttributeLength);
            success = memory_->ExtractAttributeArray(request->attributename(), attributeValue);
            if (success) {
                response->set_array_value(attributeValue.data(), attributeValue.size());
                attributeValueStr =
                    "[array of " + std::to_string(attributeValue.size()) + " bytes]";
            }
        } else if (attribute.AttributeType == "bool") {
            std::vector<uint8_t> attributeValue(1);
            success = memory_->ExtractAttributeArray(request->attributename(), attributeValue);
            bool attributeValueBool = (bool)attributeValue[0];
            if (success) {
                response->set_bool_value(attributeValueBool);
                attributeValueStr = std::to_string(attributeValueBool);
            }
        }

        if (!success) {
            spdlog::error("Failed to read {} from memory", request->attributename());
            response->set_success(false);
            response->set_message("Failed to read " + request->attributename() + " from memory");
            return Status::OK;
        }

        response->set_success(true);
        response->set_message(request->attributename() + " read successfully");
        spdlog::info("GetAttribute called - returning {} : {}", request->attributename(),
                     attributeValueStr);

        return Status::OK;
    }

    Status SetAttribute(ServerContext *context, const SetSiphonRequest *request,
                        SetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (memory_ == nullptr) {
            spdlog::error("Memory not initialized");
            response->set_success(false);
            response->set_message("Memory not initialized");
            return Status::OK;
        }

        ProcessAttribute attribute = memory_->GetAttribute(request->attributename());
        bool success = false;

        if (attribute.AttributeType == "int") {
            success = memory_->WriteAttributeInt(request->attributename(), request->int_value());
        } else if (attribute.AttributeType == "float") {
            success =
                memory_->WriteAttributeFloat(request->attributename(), request->float_value());
        } else if (attribute.AttributeType == "array") {
            const auto &arrayValue = request->array_value();
            std::vector<uint8_t> vec(arrayValue.begin(), arrayValue.end());
            success = memory_->WriteAttributeArray(request->attributename(), vec);
        } else if (attribute.AttributeType == "bool") {

            const auto &boolValue = request->bool_value();
            std::vector<uint8_t> vec(1);
            vec[0] = (uint8_t)boolValue;
            success = memory_->WriteAttributeArray(request->attributename(), vec);
        }

        if (!success) {
            spdlog::error("Failed to write {} to memory", request->attributename());
            response->set_success(false);
            response->set_message("Failed to write " + request->attributename() + " to memory");
            return Status::OK;
        }

        response->set_success(true);
        response->set_message(request->attributename() + " set successfully");

        return Status::OK;
    }

    Status InputKeyTap(ServerContext *context, const InputKeyTapRequest *request,
                       InputKeyTapResponse *response) override {
        // TODO: Add error handling
        if (input_ != 0) {
            // Convert protobuf repeated field to std::vector
            std::vector<std::string> keys(request->keys().begin(), request->keys().end());
            input_->TapKey(keys, request->hold_ms(), request->delay_ms());
            response->set_success(true);
            response->set_message("Key tapped successfully");
        } else {
            spdlog::error("Input not initialized");
            response->set_success(false);
            response->set_message("Input not initialized");
            return Status::OK;
        }
        return Status::OK;
    }

    Status InputKeyToggle(ServerContext *context, const InputKeyToggleRequest *request,
                          InputKeyToggleResponse *response) override {
        // TODO: Add error handling
        if (input_ != 0) {
            if (request->toggle()) {
                input_->PressKey(request->key());
            } else {
                input_->ReleaseKey(request->key());
            }
            response->set_success(true);
            response->set_message("Key pressed/released successfully");
        } else {
            spdlog::error("Input not initialized");
            response->set_success(false);
            response->set_message("Input not initialized");
            return Status::OK;
        }
        return Status::OK;
    }

    Status CaptureFrame(ServerContext *context, const CaptureFrameRequest *request,
                        CaptureFrameResponse *response) override {
        // TODO: Add error handling
        auto pixels = capture_->GetPixelData();
        response->set_width(capture_->processWindowWidth);
        response->set_height(capture_->processWindowHeight);
        response->set_frame(reinterpret_cast<const char *>(pixels.data()), pixels.size());
        // capture_->SaveBMP(pixels, "frame.bmp");
        spdlog::info("Frame captured successfully - width: {}, height: {}",
                     capture_->processWindowWidth, capture_->processWindowHeight);
        response->set_success(true);
        response->set_message("Frame captured successfully");

        return Status::OK;
    }

    Status MoveMouse(ServerContext *context, const MoveMouseRequest *request,
                     MoveMouseResponse *response) override {
        // TODO: Add error handling
        input_->MoveMouseSmooth(request->delta_x(), request->delta_y(), request->steps());
        response->set_success(true);
        response->set_message("Mouse moved successfully");
        return Status::OK;
    }

    Status SetProcessConfig(ServerContext *context, const SetProcessConfigRequest *request,
                            SetProcessConfigResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            processName_ = request->process_name();
            processWindowName_ = request->process_window_name();
            processAttributes_.clear();

            // Convert protobuf attributes to ProcessAttribute map
            for (const auto &attr : request->attributes()) {
                ProcessAttribute processAttr;
                processAttr.AttributeName = attr.name();
                processAttr.AttributePattern = attr.pattern();
                processAttr.AttributeOffsets.assign(attr.offsets().begin(), attr.offsets().end());
                processAttr.AttributeType = attr.type();
                processAttr.AttributeLength = static_cast<size_t>(attr.length());
                processAttr.AttributeMethod = attr.method();

                processAttributes_[attr.name()] = processAttr;
            }

            configSet_ = true;

            spdlog::info("Process configuration set: name={}, window={}, attributes={}",
                         processName_, processWindowName_, processAttributes_.size());

            response->set_success(true);
            response->set_message("Process configuration set successfully");
        } catch (const std::exception &e) {
            spdlog::error("Failed to set process config: {}", e.what());
            response->set_success(false);
            response->set_message("Failed to set process config: " + std::string(e.what()));
        }

        return Status::OK;
    }

    Status InitializeMemory(ServerContext *context, const InitializeMemoryRequest *request,
                            InitializeMemoryResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!configSet_) {
            spdlog::error("Cannot initialize memory: process config not set");
            response->set_success(false);
            response->set_message("Process configuration not set. Call SetProcessConfig first.");
            return Status::OK;
        }

        try {
            spdlog::info("Initializing memory for process: {}", processName_);

            // Create ProcessMemory instance
            memory_ = std::make_unique<ProcessMemory>(processName_, processAttributes_);

            // Initialize memory
            if (!memory_->Initialize()) {
                spdlog::error("Failed to initialize ProcessMemory");
                memory_.reset();
                response->set_success(false);
                response->set_message("Failed to initialize memory subsystem");
                return Status::OK;
            }

            // Store process ID (get it from ProcessMemory's FindProcessByName)
            processId_ = memory_->FindProcessByName(processName_);

            spdlog::info("Memory initialized successfully! Process ID: {}", processId_);

            response->set_success(true);
            response->set_message("Memory initialized successfully");
            response->set_process_id(processId_);

        } catch (const std::exception &e) {
            spdlog::error("Exception during memory initialization: {}", e.what());
            memory_.reset();
            response->set_success(false);
            response->set_message("Exception during memory initialization: " +
                                  std::string(e.what()));
        }

        return Status::OK;
    }

    Status InitializeInput(ServerContext *context, const InitializeInputRequest *request,
                           InitializeInputResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!configSet_) {
            spdlog::error("Cannot initialize input: process config not set");
            response->set_success(false);
            response->set_message("Process configuration not set. Call SetProcessConfig first.");
            return Status::OK;
        }

        try {
            // Use override window name if provided, otherwise use configured one
            std::string windowName =
                request->window_name().empty() ? processWindowName_ : request->window_name();

            spdlog::info("Initializing input for window: {}", windowName);

            // Find process window
            if (!GetProcessWindow(&windowName, &processWindow_)) {
                spdlog::error("Failed to find process window: {}", windowName);
                response->set_success(false);
                response->set_message("Failed to find process window: " + windowName);
                return Status::OK;
            }

            spdlog::info("Found process window: 0x{:X}",
                         reinterpret_cast<uintptr_t>(processWindow_));

            // Create ProcessInput instance
            input_ = std::make_unique<ProcessInput>();

            // Initialize input
            if (!input_->Initialize(processWindow_)) {
                spdlog::error("Failed to initialize ProcessInput");
                input_.reset();
                response->set_success(false);
                response->set_message("Failed to initialize input subsystem");
                return Status::OK;
            }

            // Bring window to focus
            if (BringToFocus(processWindow_)) {
                spdlog::info("Process window focused successfully!");
            } else {
                spdlog::warn("Failed to focus process window (non-critical)");
            }

            spdlog::info("Input initialized successfully!");

            response->set_success(true);
            response->set_message("Input initialized successfully");

        } catch (const std::exception &e) {
            spdlog::error("Exception during input initialization: {}", e.what());
            input_.reset();
            response->set_success(false);
            response->set_message("Exception during input initialization: " +
                                  std::string(e.what()));
        }

        return Status::OK;
    }

    Status InitializeCapture(ServerContext *context, const InitializeCaptureRequest *request,
                             InitializeCaptureResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!configSet_) {
            spdlog::error("Cannot initialize capture: process config not set");
            response->set_success(false);
            response->set_message("Process configuration not set. Call SetProcessConfig first.");
            return Status::OK;
        }

        try {
            // Use override window name if provided, otherwise use configured one
            std::string windowName =
                request->window_name().empty() ? processWindowName_ : request->window_name();

            // If window wasn't found during input init, find it now
            if (processWindow_ == nullptr) {
                spdlog::info("Finding process window for capture: {}", windowName);
                if (!GetProcessWindow(&windowName, &processWindow_)) {
                    spdlog::error("Failed to find process window: {}", windowName);
                    response->set_success(false);
                    response->set_message("Failed to find process window: " + windowName);
                    return Status::OK;
                }
            }

            spdlog::info("Initializing capture for window: 0x{:X}",
                         reinterpret_cast<uintptr_t>(processWindow_));

            // Create ProcessCapture instance
            capture_ = std::make_unique<ProcessCapture>();

            // Initialize capture
            if (!capture_->Initialize(processWindow_)) {
                spdlog::error("Failed to initialize ProcessCapture");
                capture_.reset();
                response->set_success(false);
                response->set_message("Failed to initialize capture subsystem");
                return Status::OK;
            }

            spdlog::info("Capture initialized successfully! Window size: {}x{}",
                         capture_->processWindowWidth, capture_->processWindowHeight);

            response->set_success(true);
            response->set_message("Capture initialized successfully");
            response->set_window_width(capture_->processWindowWidth);
            response->set_window_height(capture_->processWindowHeight);

        } catch (const std::exception &e) {
            spdlog::error("Exception during capture initialization: {}", e.what());
            capture_.reset();
            response->set_success(false);
            response->set_message("Exception during capture initialization: " +
                                  std::string(e.what()));
        }

        return Status::OK;
    }

    Status GetServerStatus(ServerContext *context, const GetServerStatusRequest *request,
                           GetServerStatusResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        response->set_success(true);
        response->set_message("Server status retrieved successfully");
        response->set_config_set(configSet_);
        response->set_memory_initialized(memory_ != nullptr);
        response->set_input_initialized(input_ != nullptr);
        response->set_capture_initialized(capture_ != nullptr);
        response->set_process_name(processName_);
        response->set_window_name(processWindowName_);
        response->set_process_id(processId_);

        spdlog::info("Status check - Config: {}, Memory: {}, Input: {}, Capture: {}", configSet_,
                     (memory_ != nullptr), (input_ != nullptr), (capture_ != nullptr));

        return Status::OK;
    }

    Status ExecuteCommand(ServerContext *context, const ExecuteCommandRequest *request,
                          ExecuteCommandResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto start_time = std::chrono::high_resolution_clock::now();

        // Build the command string
        std::string full_command = request->command();
        for (const auto &arg : request->args()) {
            full_command += " \"" + arg + "\"";
        }

        spdlog::info("Executing command: {}", full_command);

        try {
            // Set working directory if specified
            std::string original_dir;
            if (!request->working_directory().empty()) {
                char buffer[MAX_PATH];
                if (GetCurrentDirectoryA(MAX_PATH, buffer)) {
                    original_dir = buffer;
                }

                // Strip surrounding quotes if present
                std::string working_dir = request->working_directory();
                if (working_dir.length() >= 2 && working_dir.front() == '"' &&
                    working_dir.back() == '"') {
                    working_dir = working_dir.substr(1, working_dir.length() - 2);
                }

                if (!SetCurrentDirectoryA(working_dir.c_str())) {
                    spdlog::error("Failed to set working directory to: {}", working_dir);
                    response->set_success(false);
                    response->set_message("Failed to set working directory");
                    response->set_exit_code(-1);
                    return Status::OK;
                }
            }

            std::string stdout_output, stderr_output;
            int exit_code = 0;

            if (request->capture_output()) {
                // Execute command and capture output
                std::array<char, 128> buffer;
                std::string result;

                // Use popen to execute command and capture output
                std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(full_command.c_str(), "r"),
                                                               _pclose);
                if (!pipe) {
                    response->set_success(false);
                    response->set_message("Failed to execute command");
                    response->set_exit_code(-1);
                } else {
                    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                        result += buffer.data();
                    }
                    exit_code = _pclose(pipe.release());
                    stdout_output = result;

                    response->set_success(exit_code == 0);
                    response->set_exit_code(exit_code);
                    response->set_stdout_output(stdout_output);
                    response->set_message(exit_code == 0 ? "Command executed successfully"
                                                         : "Command failed");
                }
            } else {
                // Execute command without capturing output
                exit_code = system(full_command.c_str());
                response->set_success(exit_code == 0);
                response->set_exit_code(exit_code);
                response->set_message(exit_code == 0 ? "Command executed successfully"
                                                     : "Command failed");
            }

            // Restore original working directory
            if (!original_dir.empty()) {
                SetCurrentDirectoryA(original_dir.c_str());
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            response->set_execution_time_ms(static_cast<int32_t>(duration.count()));

            spdlog::info("Command completed with exit code: {} in {}ms", exit_code,
                         duration.count());

        } catch (const std::exception &e) {
            spdlog::error("Exception during command execution: {}", e.what());
            response->set_success(false);
            response->set_message("Exception during command execution: " + std::string(e.what()));
            response->set_exit_code(-1);
        }

        return Status::OK;
    }

    Status StartRecording(ServerContext *context, const StartRecordingRequest *request,
                          StartRecordingResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!configSet_ || !capture_ || !memory_) {
            spdlog::error("Cannot start recording: components not initialized");
            response->set_success(false);
            response->set_message("Capture and Memory must be initialized before recording");
            return Status::OK;
        }

        // Create recorder if it doesn't exist
        if (!recorder_) {
            recorder_ =
                std::make_unique<ProcessRecorder>(capture_.get(), memory_.get(), input_.get());
        }

        // Convert repeated field to vector
        std::vector<std::string> attributeNames(request->attribute_names().begin(),
                                                request->attribute_names().end());

        if (recorder_->StartRecording(attributeNames, request->output_directory(),
                                      request->max_duration_seconds())) {
            response->set_success(true);
            response->set_message("Recording started successfully");
            response->set_session_id(recorder_->GetSessionId());
            spdlog::info("Recording started - Session: {}", recorder_->GetSessionId());
        } else {
            response->set_success(false);
            response->set_message("Failed to start recording");
            spdlog::error("Failed to start recording");
        }

        return Status::OK;
    }

    Status StopRecording(ServerContext *context, const StopRecordingRequest *request,
                         StopRecordingResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!recorder_) {
            response->set_success(false);
            response->set_message("No recorder instance exists");
            return Status::OK;
        }

        RecordingStats stats;
        if (recorder_->StopRecording(stats)) {
            response->set_success(true);
            response->set_message("Recording stopped successfully");
            response->set_total_frames(stats.totalFrames);
            response->set_average_latency_ms(stats.averageLatencyMs);
            response->set_dropped_frames(stats.droppedFrames);
            response->set_actual_duration_seconds(stats.actualDurationSeconds);
            response->set_actual_fps(stats.actualFps);
            spdlog::info("Recording stopped - Frames: {}, Duration: {:.1f}s, FPS: {:.1f}, Avg "
                         "latency: {:.2f}ms, Dropped: {}",
                         stats.totalFrames, stats.actualDurationSeconds, stats.actualFps,
                         stats.averageLatencyMs, stats.droppedFrames);
        } else {
            response->set_success(false);
            response->set_message("Failed to stop recording");
        }

        return Status::OK;
    }

    Status GetRecordingStatus(ServerContext *context, const GetRecordingStatusRequest *request,
                              GetRecordingStatusResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!recorder_) {
            response->set_success(false);
            response->set_message("No recorder instance exists");
            response->set_is_recording(false);
            return Status::OK;
        }

        bool isRecording;
        int currentFrame;
        double elapsedTime;
        double currentLatency;
        int droppedFrames;

        if (recorder_->GetStatus(isRecording, currentFrame, elapsedTime, currentLatency,
                                 droppedFrames)) {
            response->set_success(true);
            response->set_message("Status retrieved successfully");
            response->set_is_recording(isRecording);
            response->set_current_frame(currentFrame);
            response->set_elapsed_time_seconds(elapsedTime);
            response->set_current_latency_ms(currentLatency);
            response->set_dropped_frames(droppedFrames);
        } else {
            response->set_success(false);
            response->set_message("Failed to get recording status");
        }

        return Status::OK;
    }

    Status DownloadRecording(ServerContext *context, const DownloadRecordingRequest *request,
                             ServerWriter<RecordingChunk> *writer) override {
        std::lock_guard<std::mutex> lock(mutex_);

        // Build path to recording file
        std::string sessionId = request->session_id();
        if (sessionId.empty()) {
            return Status(StatusCode::INVALID_ARGUMENT, "Session ID is required");
        }

        // Find the recording file (assume it's in recordings/<session_id>/recording.h5)
        std::filesystem::path recordingPath =
            std::filesystem::current_path() / "recordings" / sessionId / "recording.h5";

        if (!std::filesystem::exists(recordingPath)) {
            spdlog::error("Recording file not found: {}", recordingPath.string());
            return Status(StatusCode::NOT_FOUND,
                          "Recording file not found for session: " + sessionId);
        }

        // Open file for binary reading
        std::ifstream file(recordingPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            spdlog::error("Failed to open recording file: {}", recordingPath.string());
            return Status(StatusCode::INTERNAL, "Failed to open recording file");
        }

        // Get file size
        uint64_t totalSize = file.tellg();
        file.seekg(0, std::ios::beg);

        spdlog::info("Starting download of recording: {} ({} bytes)", sessionId, totalSize);

        // Stream file in chunks
        const size_t chunkSize = 1024 * 1024; // 1MB chunks
        std::vector<uint8_t> buffer(chunkSize);
        uint64_t offset = 0;
        size_t chunksWritten = 0;

        while (file.read(reinterpret_cast<char *>(buffer.data()), chunkSize) || file.gcount() > 0) {
            RecordingChunk chunk;
            size_t bytesRead = file.gcount();

            chunk.set_data(buffer.data(), bytesRead);
            chunk.set_offset(offset);
            chunk.set_total_size(totalSize);
            chunk.set_is_final(file.eof());
            chunk.set_filename("recording.h5");

            if (!writer->Write(chunk)) {
                spdlog::error("Failed to write chunk at offset {}", offset);
                return Status(StatusCode::INTERNAL, "Failed to stream chunk");
            }

            offset += bytesRead;
            chunksWritten++;

            // Log progress every 10 chunks (10MB)
            if (chunksWritten % 10 == 0) {
                double progress = (offset * 100.0) / totalSize;
                spdlog::info("Download progress: {:.1f}% ({}/{})", progress, offset, totalSize);
            }
        }

        file.close();
        spdlog::info("Download complete: {} chunks, {} bytes", chunksWritten, totalSize);

        return Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    SiphonServiceImpl service;

    ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024); // 100MB
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);    // 100MB
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    spdlog::info("Server listening on {}", server_address);
    spdlog::info("Waiting for client to set configuration and initialize components...");

    server->Wait();
}
