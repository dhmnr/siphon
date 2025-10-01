#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "process_input.h"
#include "process_memory.h"
#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
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
    mutable std::mutex mutex_;

  public:
    SiphonServiceImpl(ProcessMemory *memory, ProcessInput *input)
        : memory_(memory), input_(input) {}

    Status GetAttribute(ServerContext *context, const GetSiphonRequest *request,
                        GetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        int32_t attributeValue = 0;

        if (memory_ != 0) {
            // Read HP from game memory
            bool success = memory_->ExtractAttribute(request->attributename(), attributeValue);
            if (!success) {
                spdlog::error("Failed to read {} from memory", request->attributename());
                response->set_value(-1); // Error indicator
            }
        } else {
            spdlog::error("Memory not initialized or {} address not found",
                          request->attributename());
            response->set_value(-1); // Error indicator
        }

        response->set_value(attributeValue);
        spdlog::info("GetAttribute called - returning {} : {}", request->attributename(),
                     attributeValue);
        return Status::OK;
    }

    Status SetAttribute(ServerContext *context, const SetSiphonRequest *request,
                        SetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int32_t attributeValue = request->value();

        if (memory_ != 0) {
            // Write HP to game memory
            bool success = memory_->WriteAttribute(request->attributename(), attributeValue);
            if (!success) {
                spdlog::error("Failed to write {} to memory", request->attributename());
                response->set_success(false);
                response->set_message("Failed to write " + request->attributename() + " to memory");
                return Status::OK;
            }
        } else {
            spdlog::error("Memory not initialized or {} address not found",
                          request->attributename());
            response->set_success(false);
            response->set_message("Memory not initialized or " + request->attributename() +
                                  " address not found");
            return Status::OK;
        }

        response->set_success(true);
        response->set_message(request->attributename() + " set successfully");

        spdlog::info("SetAttribute called - new value: {}", attributeValue);
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
};

void RunServer(ProcessMemory *memory, ProcessInput *input) {
    std::string server_address("0.0.0.0:50051");
    SiphonServiceImpl service(memory, input);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    spdlog::info("Server listening on {}", server_address);

    server->Wait();
}

// int main() {
//     spdlog::info("Starting gRPC Variable Service Server...");
//     RunServer();
//     return 0;
// }