#include "server.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"
#include "utils.h"
#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <windows.h>

const std::string PID_FILE = "siphon.pid";

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

bool WritePidFile(DWORD pid) {
    std::ofstream pidFile(PID_FILE);
    if (!pidFile.is_open()) {
        return false;
    }
    pidFile << pid;
    pidFile.close();
    return true;
}

DWORD ReadPidFile() {
    std::ifstream pidFile(PID_FILE);
    if (!pidFile.is_open()) {
        return 0;
    }
    DWORD pid;
    pidFile >> pid;
    pidFile.close();
    return pid;
}

void DeletePidFile() { std::filesystem::remove(PID_FILE); }

bool IsProcessRunning(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        return false;
    }

    DWORD exitCode;
    bool running = GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(hProcess);
    return running;
}

int StartDaemon() {
    // Check if already running
    DWORD existingPid = ReadPidFile();
    if (existingPid != 0 && IsProcessRunning(existingPid)) {
        std::cout << "Error: Siphon server is already running (PID: " << existingPid << ")"
                  << std::endl;
        std::cout << "Use 'siphon stop' to stop it first." << std::endl;
        return 1;
    }

    // Get the current executable path
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // Build command line for background process
    std::string cmdLine = std::string("\"") + exePath + "\" --daemon-run";

    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;

    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // Create detached background process
    if (!CreateProcessA(NULL, const_cast<char *>(cmdLine.c_str()), NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL,
                        &si, &pi)) {
        std::cout << "Error: Failed to start daemon process (Error: " << GetLastError() << ")"
                  << std::endl;
        return 1;
    }

    // Write PID file
    if (!WritePidFile(pi.dwProcessId)) {
        std::cout << "Warning: Failed to write PID file" << std::endl;
    }

    std::cout << "Siphon server started in background (PID: " << pi.dwProcessId << ")" << std::endl;
    std::cout << "Logs are being written to: logs/server.log" << std::endl;
    std::cout << "Use 'siphon stop' to stop the server." << std::endl;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}

int StopDaemon() {
    DWORD pid = ReadPidFile();

    if (pid == 0) {
        std::cout << "Error: No PID file found. Server may not be running." << std::endl;
        return 1;
    }

    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        std::cout << "Error: Cannot open process (PID: " << pid
                  << "). Server may have already stopped." << std::endl;
        DeletePidFile();
        return 1;
    }

    // Check if process is still running
    DWORD exitCode;
    if (!GetExitCodeProcess(hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
        std::cout << "Error: Process (PID: " << pid << ") is not running." << std::endl;
        CloseHandle(hProcess);
        DeletePidFile();
        return 1;
    }

    // Terminate the process
    if (!TerminateProcess(hProcess, 0)) {
        std::cout << "Error: Failed to terminate process (PID: " << pid << ")" << std::endl;
        CloseHandle(hProcess);
        return 1;
    }

    // Wait for process to exit (with timeout)
    WaitForSingleObject(hProcess, 5000);
    CloseHandle(hProcess);

    DeletePidFile();
    std::cout << "Siphon server stopped (PID: " << pid << ")" << std::endl;
    return 0;
}

int StatusDaemon() {
    DWORD pid = ReadPidFile();

    if (pid == 0) {
        std::cout << "Siphon server is not running." << std::endl;
        return 0;
    }

    if (IsProcessRunning(pid)) {
        std::cout << "Siphon server is running (PID: " << pid << ")" << std::endl;
        std::cout << "Logs: logs/server.log" << std::endl;
    } else {
        std::cout << "Siphon server is not running (stale PID file found)" << std::endl;
        DeletePidFile();
    }

    return 0;
}

int RunServerNormal(bool daemon_mode) {
    InitLogger(!daemon_mode);

    if (!IsRunAsAdmin()) {
        spdlog::error("ERROR: Must run as Administrator!");
        if (!daemon_mode) {
            system("pause");
        }
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

    if (daemon_mode) {
        DeletePidFile();
    }

    return 0;
}

int main(int argc, char *argv[]) {
    CLI::App app{"Siphon Server - Remote Process Control"};
    app.require_subcommand(1); // Require exactly one subcommand
    // app.footer("\nExamples:\n"
    //            "  siphon start                [Start server in background (daemon mode)]\n"
    //            "  siphon start --foreground   [Start server in foreground (console mode)]\n"
    //            "  siphon stop                 [Stop background server]\n"
    //            "  siphon status               [Check if server is running]");

    bool daemon_run = false;
    app.add_flag("--daemon-run", daemon_run, "Internal flag for daemon mode")->group("");

    // Add subcommands
    auto start_cmd = app.add_subcommand(
        "start",
        "Start the server (background mode by default, use -f or --foreground to run in foreground)");
    bool foreground = false;
    start_cmd->add_flag("-f,--foreground", foreground,
                        "Run in foreground with console output instead of background");

    auto stop_cmd = app.add_subcommand("stop", "Stop the background server");
    auto status_cmd = app.add_subcommand("status", "Check if the server is running and show PID");

    CLI11_PARSE(app, argc, argv);

    // Handle daemon run mode (internal)
    if (daemon_run) {
        return RunServerNormal(true);
    }

    // Handle subcommands
    if (*start_cmd) {
        if (!IsRunAsAdmin()) {
            std::cout << "Error: Must run as Administrator!" << std::endl;
            return 1;
        }

        if (foreground) {
            // Run in foreground/console mode
            return RunServerNormal(false);
        } else {
            // Run in background/daemon mode (default)
            return StartDaemon();
        }
    }

    if (*stop_cmd) {
        if (!IsRunAsAdmin()) {
            std::cout << "Error: Must run as Administrator!" << std::endl;
            return 1;
        }
        return StopDaemon();
    }

    if (*status_cmd) {
        return StatusDaemon();
    }

    // Should never reach here due to require_subcommand
    return 0;
}
