/**
 * @file main.cpp
 * @brief Main entry point for slim2diretta
 *
 * Native LMS (Slimproto) player with Diretta output.
 * Mono-process architecture replacing squeezelite + squeeze2diretta-wrapper.
 */

#include "Config.h"
#include "DirettaSync.h"
#include "LogLevel.h"

#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>

#define SLIM2DIRETTA_VERSION "0.1.0"

// ============================================
// Async Logging Infrastructure
// ============================================

std::atomic<bool> g_logDrainStop{false};
std::thread g_logDrainThread;

void logDrainThreadFunc() {
    LogEntry entry;
    while (!g_logDrainStop.load(std::memory_order_acquire)) {
        while (g_logRing && g_logRing->pop(entry)) {
            std::cout << "[" << (entry.timestamp_us / 1000) << "ms] "
                      << entry.message << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Final drain on shutdown
    while (g_logRing && g_logRing->pop(entry)) {
        std::cout << "[" << (entry.timestamp_us / 1000) << "ms] "
                  << entry.message << std::endl;
    }
}

void shutdownAsyncLogging() {
    if (g_logRing) {
        g_logDrainStop.store(true, std::memory_order_release);
        if (g_logDrainThread.joinable()) {
            g_logDrainThread.join();
        }
        delete g_logRing;
        g_logRing = nullptr;
    }
}

// ============================================
// Signal Handling
// ============================================

std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nSignal " << signal << " received, shutting down..." << std::endl;
    g_running.store(false, std::memory_order_release);
}

void statsSignalHandler(int /*signal*/) {
    // TODO: Phase 5 - PlayerController::dumpStats()
    LOG_INFO("SIGUSR1 received (stats dump not yet implemented)");
}

// ============================================
// Target Listing
// ============================================

void listTargets() {
    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  Scanning for Diretta Targets...\n"
              << "═══════════════════════════════════════════════════════\n" << std::endl;

    DirettaSync::listTargets();

    std::cout << "\nUsage:\n";
    std::cout << "  Target #1: sudo ./slim2diretta -s <LMS_IP> --target 1\n";
    std::cout << "  Target #2: sudo ./slim2diretta -s <LMS_IP> --target 2\n";
    std::cout << std::endl;
}

// ============================================
// CLI Parsing
// ============================================

Config parseArguments(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if ((arg == "--server" || arg == "-s") && i + 1 < argc) {
            config.lmsServer = argv[++i];
        }
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            config.lmsPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if ((arg == "--name" || arg == "-n") && i + 1 < argc) {
            config.playerName = argv[++i];
        }
        else if ((arg == "--mac" || arg == "-m") && i + 1 < argc) {
            config.macAddress = argv[++i];
        }
        else if ((arg == "--target" || arg == "-t") && i + 1 < argc) {
            config.direttaTarget = std::atoi(argv[++i]);
            if (config.direttaTarget < 1) {
                std::cerr << "Invalid target index. Must be >= 1" << std::endl;
                exit(1);
            }
        }
        else if (arg == "--thread-mode" && i + 1 < argc) {
            config.threadMode = std::atoi(argv[++i]);
        }
        else if (arg == "--cycle-time" && i + 1 < argc) {
            config.cycleTime = static_cast<unsigned int>(std::atoi(argv[++i]));
            config.cycleTimeAuto = false;
        }
        else if (arg == "--mtu" && i + 1 < argc) {
            config.mtu = static_cast<unsigned int>(std::atoi(argv[++i]));
        }
        else if (arg == "--max-rate" && i + 1 < argc) {
            config.maxSampleRate = std::atoi(argv[++i]);
        }
        else if (arg == "--no-dsd") {
            config.dsdEnabled = false;
        }
        else if (arg == "--list-targets" || arg == "-l") {
            config.listTargets = true;
        }
        else if (arg == "--version" || arg == "-V") {
            config.showVersion = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            config.verbose = true;
        }
        else if (arg == "--quiet" || arg == "-q") {
            config.quiet = true;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "slim2diretta - Native LMS player with Diretta output\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "LMS Connection:\n"
                      << "  -s, --server <ip>      LMS server address (required)\n"
                      << "  -p, --port <port>      Slimproto port (default: 3483)\n"
                      << "  -n, --name <name>      Player name (default: slim2diretta)\n"
                      << "  -m, --mac <addr>       MAC address (default: auto-generate)\n"
                      << "\n"
                      << "Diretta:\n"
                      << "  -t, --target <index>   Diretta target index (1, 2, 3...)\n"
                      << "  -l, --list-targets     List available targets and exit\n"
                      << "  --thread-mode <mode>   SDK thread mode (default: 1)\n"
                      << "  --cycle-time <us>      Cycle time in microseconds (default: auto)\n"
                      << "  --mtu <bytes>          MTU override (default: auto)\n"
                      << "\n"
                      << "Audio:\n"
                      << "  --max-rate <hz>        Max sample rate (default: 768000)\n"
                      << "  --no-dsd               Disable DSD support\n"
                      << "\n"
                      << "Logging:\n"
                      << "  -v, --verbose          Debug output (log level: DEBUG)\n"
                      << "  -q, --quiet            Errors and warnings only (log level: WARN)\n"
                      << "\n"
                      << "Other:\n"
                      << "  -V, --version          Show version information\n"
                      << "  -h, --help             Show this help\n"
                      << "\n"
                      << "Examples:\n"
                      << "  sudo " << argv[0] << " -s 192.168.1.10 --target 1\n"
                      << "  sudo " << argv[0] << " -s 192.168.1.10 --target 1 -n \"Living Room\" -v\n"
                      << std::endl;
            exit(0);
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            exit(1);
        }
    }

    return config;
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, statsSignalHandler);

    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  slim2diretta v" << SLIM2DIRETTA_VERSION << "\n"
              << "  Native LMS player with Diretta output\n"
              << "═══════════════════════════════════════════════════════\n"
              << std::endl;

    Config config = parseArguments(argc, argv);

    // Apply log level
    if (config.verbose) {
        g_verbose = true;
        g_logLevel = LogLevel::DEBUG;
        LOG_INFO("Verbose mode enabled (log level: DEBUG)");
    } else if (config.quiet) {
        g_logLevel = LogLevel::WARN;
    }

    // Initialize async logging (only in verbose mode)
    if (config.verbose) {
        g_logRing = new LogRing();
        g_logDrainThread = std::thread(logDrainThreadFunc);
    }

    // Handle immediate actions
    if (config.showVersion) {
        std::cout << "Version:  " << SLIM2DIRETTA_VERSION << std::endl;
        std::cout << "Build:    " << __DATE__ << " " << __TIME__ << std::endl;
        shutdownAsyncLogging();
        return 0;
    }

    if (config.listTargets) {
        listTargets();
        shutdownAsyncLogging();
        return 0;
    }

    // Validate required parameters
    if (config.lmsServer.empty()) {
        std::cerr << "Error: LMS server address required (-s <ip>)" << std::endl;
        std::cerr << "Use --help for usage information" << std::endl;
        shutdownAsyncLogging();
        return 1;
    }

    if (config.direttaTarget < 1) {
        std::cerr << "Error: Diretta target required (--target <index>)" << std::endl;
        std::cerr << "Use --list-targets to see available targets" << std::endl;
        shutdownAsyncLogging();
        return 1;
    }

    // Print configuration
    std::cout << "Configuration:" << std::endl;
    std::cout << "  LMS Server: " << config.lmsServer << ":" << config.lmsPort << std::endl;
    std::cout << "  Player:     " << config.playerName << std::endl;
    std::cout << "  Target:     #" << config.direttaTarget << std::endl;
    std::cout << "  Max Rate:   " << config.maxSampleRate << " Hz" << std::endl;
    std::cout << "  DSD:        " << (config.dsdEnabled ? "enabled" : "disabled") << std::endl;
    if (!config.macAddress.empty()) {
        std::cout << "  MAC:        " << config.macAddress << std::endl;
    }
    std::cout << std::endl;

    // TODO: Phase 5 - Create and start PlayerController
    // For now, just validate that we can talk to DirettaSync
    LOG_INFO("Phase 1 scaffold - PlayerController not yet implemented");
    LOG_INFO("Use --list-targets to verify Diretta SDK connectivity");

    // Wait loop (placeholder for PlayerController)
    std::cout << "Waiting for implementation... (Press Ctrl+C to stop)" << std::endl;
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\nShutting down..." << std::endl;
    shutdownAsyncLogging();

    return 0;
}
