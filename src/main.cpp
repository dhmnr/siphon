#include "process_attribute.h"
#include "process_input.h"
#include "process_memory.h"
#include "server.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"
#include "utils.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include <map>
#include <psapi.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
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
int main(int argc, char *argv[]) {
    InitLogger(true);
    CLI::App app;
    std::string config;
    app.add_option("--config", config, "Config file path, defaults to siphon_config.toml")
        ->default_str("siphon_config.toml");
    CLI11_PARSE(app, argc, argv);

    spdlog::info("================================================");
    spdlog::info("Starting Siphon Server v0.0.1");
    spdlog::info("================================================");

    std::string processName;
    std::string processWindowName;
    std::map<std::string, ProcessAttribute> processAttributes;
    HWND processWindow;

    if (!IsRunAsAdmin()) {
        spdlog::error("ERROR: Must run as Administrator!");
        system("pause");
        return 1;
    }
    // Get attribute file from command line
    GetProcessInfoFromTOML(config, &processName, &processAttributes, &processWindowName);
    PrintProcessAttributes(processAttributes);
    spdlog::info("Process window name: {} | Process name: {}", processWindowName, processName);

    GetProcessWindow(&processWindowName, &processWindow);

    ProcessMemory memory(processName, processAttributes);
    ProcessInput input_;
    ProcessCapture capture;

    if (capture.Initialize(processWindow)) {
        spdlog::info("Process capture initialized successfully!");
    } else {
        spdlog::error("Failed to initialize process capture!");
    }
    if (memory.Initialize()) {
        spdlog::info("Process memory initialized successfully!");
    } else {
        spdlog::error("Failed to initialize process memory!");
    }
    if (input_.Initialize(processWindow)) {
        spdlog::info("Process input initialized successfully!");
    } else {
        spdlog::error("Failed to initialize process input!");
    }

    if (BringToFocus(processWindow)) {
        spdlog::info("Process window focused successfully!");
    } else {
        spdlog::error("Failed to focus process window!");
    }

    spdlog::info("Starting gRPC Variable Service Server...");
    RunServer(&memory, &input_, &capture);
    spdlog::info("================================================");
    spdlog::info("Exiting Siphon Server");
    spdlog::info("================================================");
    return 0;
}
