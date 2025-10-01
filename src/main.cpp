#include "process_attribute.h"
#include "process_input.h"
#include "process_memory.h"
#include "server.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"
#include "utils.h"
#include <iostream>
#include <map>
#include <psapi.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <tlhelp32.h>
#include <windows.h>


void InitLogger(bool use_stdout) {
    std::shared_ptr<spdlog::logger> logger;

    if (use_stdout) {
        logger = spdlog::stdout_color_mt<spdlog::async_factory>("siphon");
    } else {
        logger =
            spdlog::rotating_logger_mt<spdlog::async_factory>("siphon", "logs/server.log",
                                                              1024 * 1024 * 10, // 10MB per file
                                                              3                 // keep 3 files
            );
    }

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    spdlog::flush_every(std::chrono::seconds(3));
    spdlog::flush_on(spdlog::level::warn);
}
int main() {
    InitLogger(true);
    spdlog::info("================================================");
    spdlog::info("Starting Siphon Server");
    spdlog::info("================================================");
    std::string processName;
    std::string processWindowName;
    std::map<std::string, ProcessAttribute> processAttributes;

    if (!IsRunAsAdmin()) {
        spdlog::error("ERROR: Must run as Administrator!");
        system("pause");
        return 1;
    }
    // TODO: Get attribute file from command line
    GetProcessInfoFromTOML("attributes.toml", &processName, &processAttributes, &processWindowName);
    // PrintProcessAttributes(attributes);
    PrintProcessAttributes(processAttributes);
    spdlog::info("Process window name: {}", processWindowName);
    spdlog::info("Process name: {}", processName);

    ProcessMemory memory(processName, processAttributes);
    ProcessInput input_(processWindowName);
    if (memory.Initialize()) {
        spdlog::info("Process memory initialized successfully!");
    } else {
        spdlog::error("Failed to initialize process memory!");
    }

    spdlog::info("Starting gRPC Variable Service Server...");
    RunServer(&memory, &input_);
    spdlog::info("================================================");
    spdlog::info("Exiting Siphon Server");
    spdlog::info("================================================");
    return 0;
}
