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
    mutable std::mutex mutex_;

  public:
    SiphonServiceImpl(ProcessMemory *memory) : memory_(memory) {}

    Status GetAttribute(ServerContext *context, const GetSiphonRequest *request,
                        GetSiphonResponse *response) override {
        std::lock_guard<std::mutex> lock(mutex_);

        int32_t attributeValue = 0;

        if (memory_ != 0) {
            // Read HP from game memory
            bool success = memory_->ExtractAttribute(request->attributename(), attributeValue);
            if (!success) {
                std::cout << "Failed to read " << request->attributename() << " from memory"
                          << std::endl;
                response->set_value(-1); // Error indicator
            }
        } else {
            std::cout << "Memory not initialized or " << request->attributename()
                      << " address not found" << std::endl;
            response->set_value(-1); // Error indicator
        }

        response->set_value(attributeValue);
        std::cout << "GetAttribute called - returning " << request->attributename() << ": "
                  << attributeValue << std::endl;
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
                std::cout << "Failed to write " << request->attributename() << " to memory"
                          << std::endl;
                response->set_success(false);
                response->set_message("Failed to write " + request->attributename() + " to memory");
                return Status::OK;
            }
        } else {
            std::cout << "Memory not initialized or " << request->attributename()
                      << " address not found" << std::endl;
            response->set_success(false);
            response->set_message("Memory not initialized or " + request->attributename() +
                                  " address not found");
            return Status::OK;
        }

        response->set_success(true);
        response->set_message(request->attributename() + " set successfully");

        std::cout << "SetAttribute called - new value: " << attributeValue << std::endl;
        return Status::OK;
    }
};

void RunServer(ProcessMemory *memory) {
    std::string server_address("0.0.0.0:50051");
    SiphonServiceImpl service(memory);

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