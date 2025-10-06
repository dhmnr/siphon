#include <iostream>
#include <memory>
#include <mutex>
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
using siphon_service::GetSiphonRequest;
using siphon_service::GetSiphonResponse;
using siphon_service::InputKeyRequest;
using siphon_service::InputKeyResponse;
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

    Status InputKey(ServerContext *context, const InputKeyRequest *request,
                    InputKeyResponse *response) override {
        // TODO: Add error handling
        if (input_ != 0) {
            input_->TapKey(request->key(), std::stoi(request->value()));
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
