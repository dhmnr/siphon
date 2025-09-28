#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "process_memory.h"
#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using siphon_service::GetSiphonRequest;
using siphon_service::GetSiphonResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

class SiphonServiceImpl final : public SiphonService::Service {
  private:
    ProcessMemory *memory_;
    uintptr_t hp_address_;
    mutable std::mutex mutex_;

  public:
    SiphonServiceImpl(ProcessMemory *memory, uintptr_t hp_address)
        : memory_(memory), hp_address_(hp_address) {}

    Status GetHp(ServerContext *context, const GetSiphonRequest *request,
                 GetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        int32_t hp = 0;

        if (memory_ && hp_address_ != 0) {
            // Read HP from game memory
            bool success = memory_->ReadInt32(hp_address_, hp);
            if (!success) {
                std::cout << "Failed to read HP from memory" << std::endl;
                hp = -1; // Error indicator
            }
        } else {
            std::cout << "Memory not initialized or HP address not found" << std::endl;
            hp = -1; // Error indicator
        }

        response->set_value(hp);
        std::cout << "GetHp called - returning HP: " << hp << std::endl;
        return Status::OK;
    }

    Status SetHp(ServerContext *context, const SetSiphonRequest *request,
                 SetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t hp = request->value();

        if (memory_ && hp_address_ != 0) {
            // Write HP to game memory
            bool success = memory_->WriteInt32(hp_address_, hp);
            if (!success) {
                std::cout << "Failed to write HP to memory" << std::endl;
                response->set_success(false);
                response->set_message("Failed to write HP to memory");
                return Status::OK;
            }
        } else {
            std::cout << "Memory not initialized or HP address not found" << std::endl;
            response->set_success(false);
            response->set_message("Memory not initialized or HP address not found");
            return Status::OK;
        }

        response->set_success(true);
        response->set_message("Hp set successfully");

        std::cout << "SetHp called - new value: " << hp << std::endl;
        return Status::OK;
    }
};

void RunServer(ProcessMemory *memory, uintptr_t hp_address) {
    std::string server_address("0.0.0.0:50051");
    SiphonServiceImpl service(memory, hp_address);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
}

// int main() {
//     std::cout << "Starting gRPC Variable Service Server..." << std::endl;
//     RunServer();
//     return 0;
// }