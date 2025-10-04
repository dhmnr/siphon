#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;
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

    bool InputKey(const std::string &key, const std::string &value) {

        InputKeyRequest request;
        InputKeyResponse response;
        ClientContext context;

        request.set_key(key);
        request.set_value(value);
        Status status = stub_->InputKey(&context, request, &response);

        if (status.ok()) {
            return response.success();
        } else {
            std::cout << "InputKey RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    struct FrameData {
        std::vector<unsigned char> pixels;
        int32_t width;
        int32_t height;
        bool success;
    };

    FrameData CaptureFrame() {
        CaptureFrameRequest request;
        CaptureFrameResponse response;
        ClientContext context;

        Status status = stub_->CaptureFrame(&context, request, &response);

        FrameData result;
        if (status.ok() && response.success()) {
            result.width = response.width();
            result.height = response.height();
            result.success = true;

            // Convert the bytes to vector<unsigned char>
            result.pixels =
                std::vector<unsigned char>(response.frame().begin(), response.frame().end());
        } else {
            std::cout << "CaptureFrame RPC failed: " << status.error_message() << std::endl;
            if (!response.message().empty()) {
                std::cout << "Server message: " << response.message() << std::endl;
            }
            result.success = false;
            result.pixels = std::vector<unsigned char>();
        }
        return result;
    }

  private:
    std::unique_ptr<SiphonService::Stub> stub_;
};

bool SaveFrameToBMP(const std::vector<unsigned char> &pixels, int width, int height,
                    const std::string &filename) {

    if (pixels.empty())
        return false;

    BITMAPFILEHEADER fileHeader = {0};
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixels.size();
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER infoHeader = {0};
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = BI_RGB;

    FILE *file = fopen(filename.c_str(), "wb");
    if (!file)
        return false;

    fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    fwrite(&infoHeader, sizeof(infoHeader), 1, file);
    fwrite(pixels.data(), pixels.size(), 1, file);
    fclose(file);

    return true;
}
int main() {
    std::string server_address("localhost:50051");

    // Create a channel to the server
    SiphonClient client(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()));

    std::cout << "gRPC Siphon Client" << std::endl;
    std::cout << "Commands: get <attribute>, set <attribute> <value>, input <key> <value>, capture "
                 "<filename>, quit"
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
            std::string value;
            if (std::cin >> key >> value) {
                if (client.InputKey(key, value)) {
                    std::cout << "Key " << key << " inputted successfully" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: input <key> <value>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "capture") {
            std::string filename;
            if (std::cin >> filename) {
                auto frameData = client.CaptureFrame();

                if (frameData.success && !frameData.pixels.empty()) {
                    std::cout << "Frame captured successfully - Size: " << frameData.width << "x"
                              << frameData.height << std::endl;
                    if (SaveFrameToBMP(frameData.pixels, frameData.width, frameData.height,
                                       filename)) {
                        std::cout << "Frame saved to: " << filename << std::endl;
                    } else {
                        std::cout << "Failed to save frame to: " << filename << std::endl;
                    }
                } else {
                    std::cout << "Failed to capture frame" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: capture <filename>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else {
            std::cout << "Unknown command. Use: get <attribute>, set <attribute> <value>, input "
                         "<key> <value>, capture <filename>, or quit"
                      << std::endl;
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}