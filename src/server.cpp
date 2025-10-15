#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "process_capture.h"
#include "process_input.h"
#include "process_memory.h"
#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using siphon_service::CaptureFrameRequest;
using siphon_service::CaptureFrameResponse;
using siphon_service::ExecuteCommandRequest;
using siphon_service::ExecuteCommandResponse;
using siphon_service::GetSiphonRequest;
using siphon_service::GetSiphonResponse;
using siphon_service::InputKeyTapRequest;
using siphon_service::InputKeyTapResponse;
using siphon_service::InputKeyToggleRequest;
using siphon_service::InputKeyToggleResponse;
using siphon_service::MoveMouseRequest;
using siphon_service::MoveMouseResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

class SiphonServiceImpl final : public SiphonService::Service {
  private:
    ProcessMemory *memory_;
    ProcessInput *input_;
    ProcessCapture *capture_;
    mutable std::mutex mutex_;

  public:
    SiphonServiceImpl(ProcessMemory *memory, ProcessInput *input, ProcessCapture *capture)
        : memory_(memory), input_(input), capture_(capture) {}

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
                if (!SetCurrentDirectoryA(request->working_directory().c_str())) {
                    spdlog::error("Failed to set working directory to: {}",
                                  request->working_directory());
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
};

void RunServer(ProcessMemory *memory, ProcessInput *input, ProcessCapture *capture) {
    std::string server_address("0.0.0.0:50051");
    SiphonServiceImpl service(memory, input, capture);

    ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024); // 100MB
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);    // 100MB
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    spdlog::info("Server listening on {}", server_address);

    server->Wait();
}
