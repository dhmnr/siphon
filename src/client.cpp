#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
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
using siphon_service::InputKeyTapRequest;
using siphon_service::InputKeyTapResponse;
using siphon_service::InputKeyToggleRequest;
using siphon_service::InputKeyToggleResponse;
using siphon_service::MoveMouseRequest;
using siphon_service::MoveMouseResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

// Helper function to convert hex string to bytes
std::vector<uint8_t> HexStringToBytes(const std::string &hex) {
    std::vector<uint8_t> bytes;
    std::istringstream iss(hex);
    std::string byteStr;

    while (iss >> byteStr) {
        if (byteStr.length() == 2) {
            uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
            bytes.push_back(byte);
        }
    }
    return bytes;
}

// Helper function to convert bytes to hex string
std::string BytesToHexString(const std::string &bytes) {
    std::ostringstream oss;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0)
            oss << " ";
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(static_cast<uint8_t>(bytes[i]));
    }
    return oss.str();
}

class SiphonClient {
  public:
    SiphonClient(std::shared_ptr<Channel> channel) : stub_(SiphonService::NewStub(channel)) {}

    bool GetAttribute(const std::string &attributeName) {
        GetSiphonRequest request;
        GetSiphonResponse response;
        ClientContext context;

        request.set_attributename(attributeName);

        Status status = stub_->GetAttribute(&context, request, &response);

        if (!status.ok()) {
            std::cout << "GetAttribute RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        if (!response.success()) {
            std::cout << "Server error: " << response.message() << std::endl;
            return false;
        }

        // Handle different value types
        switch (response.value_case()) {
        case GetSiphonResponse::kIntValue:
            std::cout << attributeName << " = " << response.int_value() << " (int)" << std::endl;
            break;

        case GetSiphonResponse::kFloatValue:
            std::cout << attributeName << " = " << response.float_value() << " (float)"
                      << std::endl;
            break;

        case GetSiphonResponse::kArrayValue:
            std::cout << attributeName << " = " << BytesToHexString(response.array_value())
                      << " (array)" << std::endl;
            break;

        case GetSiphonResponse::kBoolValue:
            std::cout << attributeName << " = " << response.bool_value() << " (bool)" << std::endl;
            break;

        case GetSiphonResponse::VALUE_NOT_SET:
            std::cout << "No value returned from server" << std::endl;
            return false;
        }

        return true;
    }

    bool SetAttribute(const std::string &attributeName, const std::string &valueType,
                      const std::string &valueStr) {
        SetSiphonRequest request;
        SetSiphonResponse response;
        ClientContext context;

        request.set_attributename(attributeName);

        // Set the appropriate value type
        if (valueType == "int") {
            try {
                int32_t value = std::stoi(valueStr);
                request.set_int_value(value);
            } catch (const std::exception &e) {
                std::cout << "Invalid int value: " << valueStr << std::endl;
                return false;
            }
        } else if (valueType == "float") {
            try {
                float value = std::stof(valueStr);
                request.set_float_value(value);
            } catch (const std::exception &e) {
                std::cout << "Invalid float value: " << valueStr << std::endl;
                return false;
            }
        } else if (valueType == "array") {
            // Parse hex string (e.g., "6D DE AD BE EF")
            std::vector<uint8_t> bytes = HexStringToBytes(valueStr);
            if (bytes.empty()) {
                std::cout << "Invalid hex string: " << valueStr << std::endl;
                return false;
            }
            request.set_array_value(bytes.data(), bytes.size());
        } else if (valueType == "bool") {
            bool flag = (bool)std::stoi(valueStr);
            request.set_bool_value(flag);
        } else {
            std::cout << "Unknown value type: " << valueType << std::endl;
            return false;
        }

        Status status = stub_->SetAttribute(&context, request, &response);

        if (!status.ok()) {
            std::cout << "SetAttribute RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        std::cout << "Server response: " << response.message() << std::endl;
        return response.success();
    }

    bool InputKeyTap(const std::vector<std::string> &keys, const std::string &holdMs,
                     const std::string &delayMs) {
        InputKeyTapRequest request;
        InputKeyTapResponse response;
        ClientContext context;

        for (const auto &key : keys) {
            request.add_keys(key);
        }
        request.set_hold_ms(std::stoi(holdMs));
        request.set_delay_ms(std::stoi(delayMs));
        Status status = stub_->InputKeyTap(&context, request, &response);

        if (status.ok()) {
            return response.success();
        } else {
            std::cout << "InputKeyTap RPC failed: " << status.error_message() << std::endl;
            return false;
        }
    }

    bool InputKeyToggle(const std::string &key, const bool &toggle) {

        InputKeyToggleRequest request;
        InputKeyToggleResponse response;
        ClientContext context;
        request.set_key(key);
        request.set_toggle(toggle);
        Status status = stub_->InputKeyToggle(&context, request, &response);
        if (status.ok()) {
            return response.success();
        } else {
            std::cout << "InputKeyToggle RPC failed: " << status.error_message() << std::endl;
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

    bool MoveMouse(const int32_t &deltaX, const int32_t &deltaY, const int32_t &steps) {
        MoveMouseRequest request;
        MoveMouseResponse response;
        ClientContext context;
        request.set_delta_x(deltaX);
        request.set_delta_y(deltaY);
        request.set_steps(steps);
        Status status = stub_->MoveMouse(&context, request, &response);
        if (status.ok()) {
            return response.success();
        } else {
            std::cout << "MoveMouse RPC failed: " << status.error_message() << std::endl;
            return false;
        }
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
    infoHeader.biBitCount = 32;
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

    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(100 * 1024 * 1024); // 100MB
    args.SetMaxSendMessageSize(100 * 1024 * 1024);    // 100MB

    // Create a channel to the server
    SiphonClient client(
        grpc::CreateCustomChannel(server_address, grpc::InsecureChannelCredentials(), args));

    std::cout << "gRPC Siphon Client" << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  get <attribute>" << std::endl;
    std::cout << "  set <attribute> <type> <value>" << std::endl;
    std::cout << "    Types: int, float, array" << std::endl;
    std::cout << "    Array example: set position array \"6D DE AD BE EF\"" << std::endl;
    std::cout << "  input <key1> <key2> <key3> <value>" << std::endl;
    std::cout << "  toggle <key> <toggle>" << std::endl;
    std::cout << "  capture <filename>" << std::endl;
    std::cout << "  move <deltaX> <deltaY> <steps>" << std::endl;
    std::cout << "  quit" << std::endl;

    std::string command;
    while (true) {
        std::cout << "\n> ";
        std::cin >> command;

        if (command == "quit" || command == "q") {
            break;
        } else if (command == "get") {
            std::string attributeName;
            if (std::cin >> attributeName) {
                client.GetAttribute(attributeName);
            } else {
                std::cout << "Invalid attribute name." << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "set") {
            std::string attributeName, valueType;
            if (std::cin >> attributeName >> valueType) {
                std::string valueStr;
                std::cin.ignore(); // Ignore whitespace before getline
                std::getline(std::cin, valueStr);

                // Trim leading/trailing whitespace and quotes
                size_t start = valueStr.find_first_not_of(" \t\"");
                size_t end = valueStr.find_last_not_of(" \t\"");
                if (start != std::string::npos && end != std::string::npos) {
                    valueStr = valueStr.substr(start, end - start + 1);
                }

                client.SetAttribute(attributeName, valueType, valueStr);
            } else {
                std::cout << "Invalid input. Use: set <attribute> <type> <value>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "input") {
            std::string keysStr, holdMs, delayMs;

            if (std::cin >> keysStr >> holdMs >> delayMs) {
                // Parse space-separated keys
                std::vector<std::string> keys;
                std::stringstream ss(keysStr);
                std::string key;
                while (std::getline(ss, key, ',')) {
                    if (!key.empty()) {
                        keys.push_back(key);
                    }
                }

                if (client.InputKeyTap(keys, holdMs, delayMs)) {
                    std::cout << "Keys ";
                    for (size_t i = 0; i < keys.size(); ++i) {
                        std::cout << keys[i];
                        if (i < keys.size() - 1)
                            std::cout << ",";
                    }
                    std::cout << " inputted successfully" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: input <keys> <value>" << std::endl;
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
        } else if (command == "move") {
            std::string deltaX, deltaY, steps;
            if (std::cin >> deltaX >> deltaY >> steps) {
                if (client.MoveMouse(std::stoi(deltaX), std::stoi(deltaY), std::stoi(steps))) {
                    std::cout << "Mouse moved successfully" << std::endl;
                } else {
                    std::cout << "Failed to move mouse" << std::endl;
                }
            }
        }

        else if (command == "toggle") {
            std::string key;
            bool toggle = false;
            if (std::cin >> key >> toggle) {
                if (client.InputKeyToggle(key, toggle)) {
                    std::cout << "Key " << key << " " << (toggle ? "pressed" : "released")
                              << " successfully" << std::endl;
                } else {
                    std::cout << "Failed to toggle key" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: toggle <key> <toggle>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else {
            std::cout << "Unknown command. Type 'quit' to exit or see commands above." << std::endl;
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}