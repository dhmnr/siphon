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
using siphon_service::InputKeyRequest;
using siphon_service::InputKeyResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

class SiphonClient {
  public:
    SiphonClient(std::shared_ptr<Channel> channel) : stub_(SiphonService::NewStub(channel)) {}

    int32_t GetAttribute(const std::string &attributeName) {
        GetSiphonRequest request;
        GetSiphonResponse response;
        ClientContext context;

        request.set_attributename(attributeName);

        Status status = stub_->GetAttribute(&context, request, &response);

        if (status.ok()) {
            return response.value();
        } else {
            std::cout << "GetAttribute RPC failed: " << status.error_message() << std::endl;
            return -1; // Error indicator
        }
    }

    bool SetAttribute(const std::string &attributeName, int32_t value) {
        SetSiphonRequest request;
        SetSiphonResponse response;
        ClientContext context;

        request.set_value(value);
        request.set_attributename(attributeName);

        Status status = stub_->SetAttribute(&context, request, &response);

        if (status.ok()) {
            std::cout << "Server response: " << response.message() << std::endl;
            return response.success();
        } else {
            std::cout << "SetAttribute RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool InputKey(const std::string &key) {

        InputKeyRequest request;
        InputKeyResponse response;
        ClientContext context;

        request.set_key(key);

        Status status = stub_->InputKey(&context, request, &response);

        if (status.ok()) {
            return response.success();
        } else {
            std::cout << "InputKey RPC failed: " << status.error_message() << std::endl;
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
    std::cout << "Commands: get <attribute>, set <attribute> <value>, input <key>, quit"
              << std::endl;

    std::string command;
    while (true) {
        std::cout << "\n> ";
        std::cin >> command;

        if (command == "quit" || command == "q") {
            break;
        } else if (command == "get") {
            std::string attributeName;
            if (std::cin >> attributeName) {
                int32_t value = client.GetAttribute(attributeName);
                if (value != -1) {
                    std::cout << attributeName << " value: " << value << std::endl;
                }
            } else {
                std::cout << "Invalid attribute name." << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "set") {
            std::string attributeName;
            int32_t value;
            if (std::cin >> attributeName >> value) {
                if (client.SetAttribute(attributeName, value)) {
                    std::cout << attributeName << " set to: " << value << std::endl;
                } else {
                    std::cout << "Failed to set " << attributeName << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: set <attribute> <value>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "input") {
            std::string key;
            if (std::cin >> key) {
                if (client.InputKey(key)) {
                    std::cout << "Key " << key << " inputted successfully" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: input <key>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        }

        else {
            std::cout << "Unknown command. Use: get <attribute>, set <attribute> <value>, or quit"
                      << std::endl;
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}