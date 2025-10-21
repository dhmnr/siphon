#include "server.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"
#include "utils.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
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
    CLI::App app{"Siphon Server - Remote Process Control"};
    CLI11_PARSE(app, argc, argv);

    if (!IsRunAsAdmin()) {
        spdlog::error("ERROR: Must run as Administrator!");
        system("pause");
        return 1;
    }

    spdlog::info("================================================");
    spdlog::info("Starting Siphon Server v0.0.2");
    spdlog::info("================================================");
    spdlog::info("Server will start without target process.");
    spdlog::info("Use client to configure and initialize components.");
    spdlog::info("================================================");

    spdlog::info("Starting gRPC Server...");
    RunServer();

    spdlog::info("================================================");
    spdlog::info("Exiting Siphon Server");
    spdlog::info("================================================");
    return 0;
}
