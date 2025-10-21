#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "siphon_service.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <toml++/toml.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using siphon_service::CaptureFrameRequest;
using siphon_service::CaptureFrameResponse;
using siphon_service::ExecuteCommandRequest;
using siphon_service::ExecuteCommandResponse;
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
using siphon_service::SetProcessConfigRequest;
using siphon_service::SetProcessConfigResponse;
using siphon_service::SetSiphonRequest;
using siphon_service::SetSiphonResponse;
using siphon_service::SiphonService;

// Helper function to parse TOML config and build protobuf request
bool ParseConfigFile(const std::string &filepath, std::string &processName,
                     std::string &processWindowName,
                     siphon_service::SetProcessConfigRequest &request) {
    try {
        // Parse TOML file
        auto config = toml::parse_file(filepath);

        // Get process info
        auto processInfo = config["process_info"];
        if (!processInfo) {
            std::cerr << "Missing [process_info] section in config" << std::endl;
            return false;
        }

        processName = processInfo["name"].value_or("");
        processWindowName = processInfo["window_name"].value_or("");

        if (processName.empty()) {
            std::cerr << "Missing 'name' in [process_info]" << std::endl;
            return false;
        }

        request.set_process_name(processName);
        request.set_process_window_name(processWindowName);

        // Get attributes
        auto attributes = config["attributes"];
        if (!attributes) {
            std::cerr << "Missing [attributes] section in config" << std::endl;
            return false;
        }

        // Parse each attribute
        for (auto &&[name, attr] : *attributes.as_table()) {
            // Each attr is a toml::node, convert to table
            auto attrTable = attr.as_table();
            if (!attrTable) {
                continue; // Skip if not a table
            }

            auto *protoAttr = request.add_attributes();
            protoAttr->set_name(std::string(name));

            // Get pattern
            if (auto pattern = (*attrTable)["pattern"].value<std::string>()) {
                protoAttr->set_pattern(*pattern);
            }

            // Get offsets
            if (auto offsetsArray = (*attrTable)["offsets"].as_array()) {
                for (auto &&offset : *offsetsArray) {
                    if (auto val = offset.value<int64_t>()) {
                        protoAttr->add_offsets(static_cast<uint64_t>(*val));
                    }
                }
            }

            // Get type
            if (auto type = (*attrTable)["type"].value<std::string>()) {
                protoAttr->set_type(*type);
            }

            // Get length (optional, default to 0)
            if (auto length = (*attrTable)["length"].value<int64_t>()) {
                protoAttr->set_length(static_cast<uint64_t>(*length));
            } else {
                protoAttr->set_length(0);
            }

            // Get method (optional, default to empty)
            if (auto method = (*attrTable)["method"].value<std::string>()) {
                protoAttr->set_method(*method);
            } else {
                protoAttr->set_method("");
            }
        }

        return true;

    } catch (const toml::parse_error &err) {
        std::cerr << "TOML parse error: " << err.description() << std::endl;
        return false;
    } catch (const std::exception &e) {
        std::cerr << "Error parsing config: " << e.what() << std::endl;
        return false;
    }
}

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

    struct CommandResult {
        bool success;
        std::string message;
        int32_t exit_code;
        std::string stdout_output;
        std::string stderr_output;
        int32_t execution_time_ms;
    };

    CommandResult ExecuteCommand(const std::string &command,
                                 const std::vector<std::string> &args = {},
                                 const std::string &working_directory = "",
                                 int32_t timeout_seconds = 30, bool capture_output = true) {
        ExecuteCommandRequest request;
        ExecuteCommandResponse response;
        ClientContext context;

        request.set_command(command);
        for (const auto &arg : args) {
            request.add_args(arg);
        }
        request.set_working_directory(working_directory);
        request.set_timeout_seconds(timeout_seconds);
        request.set_capture_output(capture_output);

        Status status = stub_->ExecuteCommand(&context, request, &response);

        CommandResult result;
        if (status.ok()) {
            result.success = response.success();
            result.message = response.message();
            result.exit_code = response.exit_code();
            result.stdout_output = response.stdout_output();
            result.stderr_output = response.stderr_output();
            result.execution_time_ms = response.execution_time_ms();
        } else {
            std::cout << "ExecuteCommand RPC failed: " << status.error_message() << std::endl;
            result.success = false;
            result.message = "RPC failed: " + status.error_message();
            result.exit_code = -1;
            result.execution_time_ms = 0;
        }

        return result;
    }

    bool SetProcessConfig(const SetProcessConfigRequest &request) {
        SetProcessConfigResponse response;
        ClientContext context;

        Status status = stub_->SetProcessConfig(&context, request, &response);

        if (!status.ok()) {
            std::cout << "SetProcessConfig RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        std::cout << "Server response: " << response.message() << std::endl;
        return response.success();
    }

    bool InitializeMemory() {
        InitializeMemoryRequest request;
        InitializeMemoryResponse response;
        ClientContext context;

        Status status = stub_->InitializeMemory(&context, request, &response);

        if (!status.ok()) {
            std::cout << "InitializeMemory RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        std::cout << "Server response: " << response.message() << std::endl;
        if (response.success()) {
            std::cout << "Process ID: " << response.process_id() << std::endl;
        }
        return response.success();
    }

    bool InitializeInput(const std::string &windowName = "") {
        InitializeInputRequest request;
        InitializeInputResponse response;
        ClientContext context;

        if (!windowName.empty()) {
            request.set_window_name(windowName);
        }

        Status status = stub_->InitializeInput(&context, request, &response);

        if (!status.ok()) {
            std::cout << "InitializeInput RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        std::cout << "Server response: " << response.message() << std::endl;
        return response.success();
    }

    bool InitializeCapture(const std::string &windowName = "") {
        InitializeCaptureRequest request;
        InitializeCaptureResponse response;
        ClientContext context;

        if (!windowName.empty()) {
            request.set_window_name(windowName);
        }

        Status status = stub_->InitializeCapture(&context, request, &response);

        if (!status.ok()) {
            std::cout << "InitializeCapture RPC failed: " << status.error_message() << std::endl;
            return false;
        }

        std::cout << "Server response: " << response.message() << std::endl;
        if (response.success()) {
            std::cout << "Window size: " << response.window_width() << "x"
                      << response.window_height() << std::endl;
        }
        return response.success();
    }

    struct ServerStatus {
        bool success;
        std::string message;
        bool config_set;
        bool memory_initialized;
        bool input_initialized;
        bool capture_initialized;
        std::string process_name;
        std::string window_name;
        int32_t process_id;
    };

    ServerStatus GetServerStatus() {
        GetServerStatusRequest request;
        GetServerStatusResponse response;
        ClientContext context;

        Status status = stub_->GetServerStatus(&context, request, &response);

        ServerStatus result;
        if (status.ok()) {
            result.success = response.success();
            result.message = response.message();
            result.config_set = response.config_set();
            result.memory_initialized = response.memory_initialized();
            result.input_initialized = response.input_initialized();
            result.capture_initialized = response.capture_initialized();
            result.process_name = response.process_name();
            result.window_name = response.window_name();
            result.process_id = response.process_id();
        } else {
            std::cout << "GetServerStatus RPC failed: " << status.error_message() << std::endl;
            result.success = false;
            result.message = "RPC failed: " + status.error_message();
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

    std::cout << "gRPC Siphon Client v0.0.2" << std::endl;
    std::cout << "\n=== Initialization Commands ===" << std::endl;
    std::cout << "  init <config_file>        - Load config and initialize all components"
              << std::endl;
    std::cout << "  status                    - Show server initialization status" << std::endl;
    std::cout << "  config <config_file>      - Load and send config to server" << std::endl;
    std::cout << "  init-memory               - Initialize memory subsystem" << std::endl;
    std::cout << "  init-input [window_name]  - Initialize input subsystem" << std::endl;
    std::cout << "  init-capture [window_name]- Initialize capture subsystem" << std::endl;
    std::cout << "\n=== Control Commands ===" << std::endl;
    std::cout << "  get <attribute>           - Get attribute value" << std::endl;
    std::cout << "  set <attribute> <type> <value> - Set attribute (int, float, array, bool)"
              << std::endl;
    std::cout << "  input <key1> <key2> <key3> <value> - Tap keys" << std::endl;
    std::cout << "  toggle <key> <toggle>     - Press/release key" << std::endl;
    std::cout << "  capture <filename>        - Capture frame to BMP file" << std::endl;
    std::cout << "  move <deltaX> <deltaY> <steps> - Move mouse" << std::endl;
    std::cout << "  exec <command> [args...]  - Execute command on server" << std::endl;
    std::cout << "  quit                      - Exit client" << std::endl;

    std::string command;
    while (true) {
        std::cout << "\n> ";
        std::cin >> command;

        if (command == "quit" || command == "q") {
            break;
        } else if (command == "init") {
            std::string configFile;
            if (std::cin >> configFile) {
                std::cout << "Loading config from: " << configFile << std::endl;

                // Parse TOML config directly into protobuf request
                std::string processName, processWindowName;
                SetProcessConfigRequest configRequest;

                if (!ParseConfigFile(configFile, processName, processWindowName, configRequest)) {
                    std::cout << "Failed to load config file: " << configFile << std::endl;
                    std::cin.clear();
                    std::cin.ignore(10000, '\n');
                    continue;
                }

                std::cout << "Config loaded - Process: " << processName
                          << ", Window: " << processWindowName
                          << ", Attributes: " << configRequest.attributes_size() << std::endl;

                // Send config to server
                std::cout << "Sending configuration to server..." << std::endl;
                if (!client.SetProcessConfig(configRequest)) {
                    std::cout << "Failed to set process config" << std::endl;
                    continue;
                }

                // Wait a moment for process to be running
                std::cout << "Waiting for process to be ready..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));

                // Initialize memory
                std::cout << "Initializing memory subsystem..." << std::endl;
                if (!client.InitializeMemory()) {
                    std::cout << "Failed to initialize memory" << std::endl;
                    continue;
                }

                // Initialize input
                std::cout << "Initializing input subsystem..." << std::endl;
                if (!client.InitializeInput()) {
                    std::cout << "Failed to initialize input" << std::endl;
                    continue;
                }

                // Initialize capture
                std::cout << "Initializing capture subsystem..." << std::endl;
                if (!client.InitializeCapture()) {
                    std::cout << "Failed to initialize capture" << std::endl;
                    continue;
                }

                std::cout << "\n=== Initialization Complete! ===" << std::endl;
                std::cout << "All subsystems initialized successfully." << std::endl;

            } else {
                std::cout << "Invalid input. Use: init <config_file>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "status") {
            auto status = client.GetServerStatus();
            if (status.success) {
                std::cout << "\n=== Server Status ===" << std::endl;
                std::cout << "Config Set:          " << (status.config_set ? "Yes" : "No")
                          << std::endl;
                std::cout << "Memory Initialized:  " << (status.memory_initialized ? "Yes" : "No")
                          << std::endl;
                std::cout << "Input Initialized:   " << (status.input_initialized ? "Yes" : "No")
                          << std::endl;
                std::cout << "Capture Initialized: " << (status.capture_initialized ? "Yes" : "No")
                          << std::endl;
                if (status.config_set) {
                    std::cout << "Process Name:        " << status.process_name << std::endl;
                    std::cout << "Window Name:         " << status.window_name << std::endl;
                    if (status.process_id > 0) {
                        std::cout << "Process ID:          " << status.process_id << std::endl;
                    }
                }
                std::cout << "Message: " << status.message << std::endl;
            } else {
                std::cout << "Failed to get server status" << std::endl;
            }
        } else if (command == "config") {
            std::string configFile;
            if (std::cin >> configFile) {
                std::cout << "Loading config from: " << configFile << std::endl;

                std::string processName, processWindowName;
                SetProcessConfigRequest configRequest;

                if (!ParseConfigFile(configFile, processName, processWindowName, configRequest)) {
                    std::cout << "Failed to load config file: " << configFile << std::endl;
                    continue;
                }

                std::cout << "Config loaded - Process: " << processName
                          << ", Window: " << processWindowName
                          << ", Attributes: " << configRequest.attributes_size() << std::endl;

                if (client.SetProcessConfig(configRequest)) {
                    std::cout << "Configuration sent to server successfully" << std::endl;
                } else {
                    std::cout << "Failed to send configuration" << std::endl;
                }
            } else {
                std::cout << "Invalid input. Use: config <config_file>" << std::endl;
                std::cin.clear();
                std::cin.ignore(10000, '\n');
            }
        } else if (command == "init-memory") {
            std::cout << "Initializing memory subsystem..." << std::endl;
            if (client.InitializeMemory()) {
                std::cout << "Memory subsystem initialized successfully" << std::endl;
            } else {
                std::cout << "Failed to initialize memory subsystem" << std::endl;
            }
        } else if (command == "init-input") {
            std::string windowName;
            std::cin >> std::ws; // Skip whitespace
            std::getline(std::cin, windowName);
            windowName = windowName.empty() ? "" : windowName;

            std::cout << "Initializing input subsystem..." << std::endl;
            if (client.InitializeInput(windowName)) {
                std::cout << "Input subsystem initialized successfully" << std::endl;
            } else {
                std::cout << "Failed to initialize input subsystem" << std::endl;
            }
        } else if (command == "init-capture") {
            std::string windowName;
            std::cin >> std::ws; // Skip whitespace
            std::getline(std::cin, windowName);
            windowName = windowName.empty() ? "" : windowName;

            std::cout << "Initializing capture subsystem..." << std::endl;
            if (client.InitializeCapture(windowName)) {
                std::cout << "Capture subsystem initialized successfully" << std::endl;
            } else {
                std::cout << "Failed to initialize capture subsystem" << std::endl;
            }
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
        } else if (command == "exec") {
            std::string line;
            std::cin.ignore(); // Ignore whitespace before getline
            std::getline(std::cin, line);

            // Parse command line arguments with support for quoted strings
            std::vector<std::string> tokens;
            std::string token;
            bool inQuotes = false;
            bool escape = false;

            for (size_t i = 0; i < line.length(); ++i) {
                char c = line[i];

                if (escape) {
                    token += c;
                    escape = false;
                    continue;
                }

                if (c == '\\' && i + 1 < line.length() && line[i + 1] == '"') {
                    escape = true;
                    continue;
                }

                if (c == '"') {
                    inQuotes = !inQuotes;
                    continue;
                }

                if (c == ' ' && !inQuotes) {
                    if (!token.empty()) {
                        tokens.push_back(token);
                        token.clear();
                    }
                    continue;
                }

                token += c;
            }

            if (!token.empty()) {
                tokens.push_back(token);
            }

            if (tokens.empty()) {
                std::cout << "Invalid input. Use: exec <command> [args...]" << std::endl;
                continue;
            }

            std::string cmd = tokens[0];
            std::vector<std::string> args;
            std::string working_dir = "";
            int32_t timeout = 30;
            bool capture_output = true;

            // Parse arguments and options
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (tokens[i] == "--dir" && i + 1 < tokens.size()) {
                    working_dir = tokens[++i];
                } else if (tokens[i] == "--timeout" && i + 1 < tokens.size()) {
                    timeout = std::stoi(tokens[++i]);
                } else if (tokens[i] == "--no-capture") {
                    capture_output = false;
                } else {
                    args.push_back(tokens[i]);
                }
            }

            std::cout << "Executing command: " << cmd;
            for (const auto &arg : args) {
                std::cout << " " << arg;
            }
            std::cout << std::endl;

            auto result = client.ExecuteCommand(cmd, args, working_dir, timeout, capture_output);

            std::cout << "Command completed:" << std::endl;
            std::cout << "  Success: " << (result.success ? "true" : "false") << std::endl;
            std::cout << "  Exit Code: " << result.exit_code << std::endl;
            std::cout << "  Execution Time: " << result.execution_time_ms << "ms" << std::endl;
            std::cout << "  Message: " << result.message << std::endl;

            if (!result.stdout_output.empty()) {
                std::cout << "  Output:" << std::endl;
                std::cout << result.stdout_output << std::endl;
            }

            if (!result.stderr_output.empty()) {
                std::cout << "  Error Output:" << std::endl;
                std::cout << result.stderr_output << std::endl;
            }
        } else {
            std::cout << "Unknown command. Type 'quit' to exit or see commands above." << std::endl;
        }
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}