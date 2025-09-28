#include <iostream>
#include <memory>
#include <string>

#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using siphon_service::GetSiphonRequest;
using siphon_service::GetSiphonResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

class SiphonClient {
  public:
    SiphonClient(std::shared_ptr<Channel> channel) : stub_(SiphonService::NewStub(channel)) {}

    int32_t GetHp() {
        GetSiphonRequest request;
        GetSiphonResponse response;
        ClientContext context;

        Status status = stub_->GetHp(&context, request, &response);

        if (status.ok()) {
            return response.value();
        } else {
            std::cout << "GetHp RPC failed: " << status.error_message() << std::endl;
            return -1; // Error indicator
        }
    }

    bool SetHp(int32_t value) {
        SetSiphonRequest request;
        SetSiphonResponse response;
        ClientContext context;

        request.set_value(value);

        Status status = stub_->SetHp(&context, request, &response);

        if (status.ok()) {
            std::cout << "Server response: " << response.message() << std::endl;
            return response.success();
        } else {
            std::cout << "SetHp RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

  private:
    std::unique_ptr<SiphonService::Stub> stub_;
};

int main() {
    std::string server_address("localhost:50051");

    // Create a channel to the server
    SiphonClient client(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()));

    std::cout << "gRPC Siphon Client" << std::endl;
    std::cout << "Commands: get, set <value>, quit" << std::endl;

    std::string command;
    while (true) {
        std::cout << "\n> ";
        std::cin >> command;

        if (command == "quit" || command == "q") {
            break;
        } else if (command == "get") {
            int32_t value = client.GetHp();
            if (value != -1) {
                std::cout << "Variable value: " << value << std::endl;
            }
        } else if (command == "set") {
            int32_t value;
            if (std::cin >> value) {
                if (client.SetHp(value)) {
                    std::cout << "Variable set to: " << value << std::endl;
                } else {
                    std::cout << "Failed to set variable" << std::endl;
                }
            } else {
                std::cout << "Invalid value. Please enter a number." << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else {
            std::cout << "Unknown command. Use: get, set <value>, or quit" << std::endl;
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}