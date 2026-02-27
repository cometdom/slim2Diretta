/**
 * @file main.cpp
 * @brief Main entry point for slim2diretta
 *
 * Native LMS (Slimproto) player with Diretta output.
 * Mono-process architecture replacing squeezelite + squeeze2diretta-wrapper.
 */

#include "Config.h"
#include "SlimprotoClient.h"
#include "HttpStreamClient.h"
#include "Decoder.h"
#include "DsdStreamReader.h"
#include "DsdProcessor.h"
#include "DirettaSync.h"
#include "LogLevel.h"

#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

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
SlimprotoClient* g_slimproto = nullptr;  // For signal handler access
DirettaSync* g_diretta = nullptr;        // For SIGUSR1 stats dump

void signalHandler(int signal) {
    std::cout << "\nSignal " << signal << " received, shutting down..." << std::endl;
    g_running.store(false, std::memory_order_release);
    // Stop the slimproto client to unblock its receive loop
    if (g_slimproto) {
        g_slimproto->stop();
    }
}

void statsSignalHandler(int /*signal*/) {
    if (g_diretta) {
        g_diretta->dumpStats();
    }
}

// ============================================
// LMS Autodiscovery
// ============================================

/**
 * @brief Discover LMS server via UDP broadcast on port 3483
 *
 * Sends 'e' packet as broadcast, LMS responds from its IP.
 * Same method as squeezelite (MIT reference).
 *
 * @param timeoutSec Timeout per attempt in seconds
 * @param retries Number of discovery attempts
 * @return Server IP as string, or empty on failure
 */
std::string discoverLMS(int timeoutSec = 5, int retries = 3) {
    for (int attempt = 0; attempt < retries; attempt++) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return "";

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(3483);
        bcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        const char msg = 'e';
        sendto(sock, &msg, 1, 0,
               reinterpret_cast<struct sockaddr*>(&bcastAddr), sizeof(bcastAddr));

        struct pollfd pfd = {sock, POLLIN, 0};
        if (poll(&pfd, 1, timeoutSec * 1000) > 0) {
            char buf[32];
            struct sockaddr_in serverAddr{};
            socklen_t slen = sizeof(serverAddr);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                                 reinterpret_cast<struct sockaddr*>(&serverAddr), &slen);
            ::close(sock);
            if (n > 0) {
                std::string ip = inet_ntoa(serverAddr.sin_addr);
                LOG_INFO("Discovered LMS at " << ip
                         << " (attempt " << (attempt + 1) << ")");
                return ip;
            }
        }
        ::close(sock);

        if (attempt < retries - 1) {
            LOG_DEBUG("Discovery attempt " << (attempt + 1) << " timed out, retrying...");
        }
    }
    return "";
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
                      << "  -s, --server <ip>      LMS server address (auto-discover if omitted)\n"
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
                      << "  sudo " << argv[0] << " --target 1                              # Auto-discover LMS\n"
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

    // Validate required parameters — autodiscover LMS if not specified
    if (config.lmsServer.empty()) {
        std::cout << "No LMS server specified, searching..." << std::endl;
        config.lmsServer = discoverLMS();
        if (config.lmsServer.empty()) {
            std::cerr << "Error: Could not discover LMS server" << std::endl;
            std::cerr << "Specify manually with -s <ip>" << std::endl;
            shutdownAsyncLogging();
            return 1;
        }
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

    // Create and enable DirettaSync
    auto diretta = std::make_unique<DirettaSync>();
    diretta->setTargetIndex(config.direttaTarget - 1);  // CLI 1-indexed → API 0-indexed
    if (config.mtu > 0) diretta->setMTU(config.mtu);

    DirettaConfig direttaConfig;
    direttaConfig.threadMode = config.threadMode;
    direttaConfig.cycleTime = config.cycleTime;
    direttaConfig.cycleTimeAuto = config.cycleTimeAuto;
    if (config.mtu > 0) direttaConfig.mtu = config.mtu;

    if (!diretta->enable(direttaConfig)) {
        std::cerr << "Failed to enable Diretta target #" << config.direttaTarget << std::endl;
        shutdownAsyncLogging();
        return 1;
    }
    g_diretta = diretta.get();
    DirettaSync* direttaPtr = diretta.get();  // For lambda captures

    std::cout << "Diretta target #" << config.direttaTarget << " enabled" << std::endl;

    // Create Slimproto client and connect to LMS
    auto slimproto = std::make_unique<SlimprotoClient>();
    g_slimproto = slimproto.get();

    // HTTP stream client (shared between callbacks and potential audio thread)
    auto httpStream = std::make_shared<HttpStreamClient>();
    std::thread audioTestThread;
    std::atomic<bool> audioTestRunning{false};
    std::atomic<bool> audioThreadDone{true};  // true when no thread is running

    // Register stream callback
    slimproto->onStream([&](const StrmCommand& cmd, const std::string& httpRequest) {
        switch (cmd.command) {
            case STRM_START: {
                LOG_INFO("Stream start requested (format=" << cmd.format << ")");

                // Stop previous playback
                if (direttaPtr->isPlaying()) {
                    direttaPtr->stopPlayback(true);
                }

                // Stop any previous audio thread
                audioTestRunning.store(false);
                httpStream->disconnect();
                if (audioTestThread.joinable()) {
                    // Wait up to 500ms for the thread to finish
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    while (!audioThreadDone.load(std::memory_order_acquire) &&
                           std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if (audioThreadDone.load(std::memory_order_acquire)) {
                        audioTestThread.join();
                    } else {
                        audioTestThread.detach();
                        LOG_WARN("Audio thread did not stop in time, detached");
                    }
                }

                // Determine server IP (0 = use control connection IP)
                std::string streamIp = slimproto->getServerIp();
                if (cmd.serverIp != 0) {
                    struct in_addr addr;
                    addr.s_addr = cmd.serverIp;  // Already in network byte order
                    streamIp = inet_ntoa(addr);
                }
                uint16_t streamPort = cmd.getServerPort();
                if (streamPort == 0) streamPort = SLIMPROTO_HTTP_PORT;

                // Connect HTTP stream
                if (!httpStream->connect(streamIp, streamPort, httpRequest)) {
                    LOG_ERROR("Failed to connect to audio stream");
                    slimproto->sendStat(StatEvent::STMn);
                    break;
                }

                // Send STAT sequence to LMS
                slimproto->sendStat(StatEvent::STMc);  // Connected
                slimproto->sendResp(httpStream->getResponseHeaders());
                slimproto->sendStat(StatEvent::STMh);  // Headers received

                // Reset elapsed time for new track
                slimproto->updateElapsed(0, 0);
                slimproto->updateStreamBytes(0);

                // Start audio decode thread
                char formatCode = cmd.format;
                char pcmRate = cmd.pcmSampleRate;
                char pcmSize = cmd.pcmSampleSize;
                char pcmChannels = cmd.pcmChannels;
                char pcmEndian = cmd.pcmEndian;
                audioTestRunning.store(true);
                audioThreadDone.store(false, std::memory_order_release);
                audioTestThread = std::thread([&httpStream, &slimproto, &audioTestRunning, &audioThreadDone, formatCode, pcmRate, pcmSize, pcmChannels, pcmEndian, direttaPtr]() {

                    // ============================================================
                    // DSD PATH — separate from PCM/FLAC
                    // ============================================================
                    if (formatCode == FORMAT_DSD) {
                        auto dsdReader = std::make_unique<DsdStreamReader>();

                        // Set raw DSD format hint from strm params (fallback for raw DSD)
                        uint32_t hintRate = sampleRateFromCode(pcmRate);
                        uint32_t hintCh = (pcmChannels == '2') ? 2
                                        : (pcmChannels == '1') ? 1 : 2;
                        if (hintRate > 0) {
                            dsdReader->setRawDsdFormat(hintRate, hintCh);
                        }

                        slimproto->sendStat(StatEvent::STMs);

                        uint8_t httpBuf[65536];
                        uint64_t totalBytes = 0;
                        bool formatLogged = false;
                        uint64_t lastElapsedLog = 0;

                        // Planar buffer: readPlanar() fills this, sendAudio() consumes it directly.
                        // No intermediate cache — each readPlanar output is a self-contained
                        // planar chunk that must be sent as-is to preserve [L...][R...] structure.
                        //
                        // CRITICAL: Keep this small! pushDSDPlanarOptimized computes the R channel
                        // offset from the pushed size, not the input size. If the ring buffer
                        // doesn't have room for the full chunk, a partial push reads R data from
                        // the wrong position (inside L data). Small chunks avoid this by always
                        // fitting in the ring buffer's free space.
                        constexpr size_t DSD_PLANAR_BUF = 16384;  // ~2 block groups max
                        uint8_t planarBuf[DSD_PLANAR_BUF];

                        constexpr unsigned int PREBUFFER_MS = 500;
                        uint64_t pushedDsdBytes = 0;
                        bool direttaOpened = false;
                        AudioFormat audioFmt{};
                        uint32_t detectedChannels = 2;
                        uint32_t dsdBitRate = 0;
                        uint64_t byteRateTotal = 0;

                        bool httpEof = false;
                        while (audioTestRunning.load(std::memory_order_acquire) &&
                               (!httpEof || dsdReader->availableBytes() > 0 ||
                                !dsdReader->isFinished())) {

                            // === PHASE 1: HTTP read + feed ===
                            // Flow control: don't read HTTP when internal buffer is large
                            constexpr size_t DSD_BUF_MAX = 1048576;  // 1MB max
                            bool gotData = false;
                            if (!httpEof && dsdReader->availableBytes() < DSD_BUF_MAX) {
                                if (httpStream->isConnected()) {
                                    ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 2);
                                    if (n > 0) {
                                        gotData = true;
                                        totalBytes += n;
                                        slimproto->updateStreamBytes(totalBytes);
                                        dsdReader->feed(httpBuf, static_cast<size_t>(n));
                                    } else if (n < 0 || !httpStream->isConnected()) {
                                        httpEof = true;
                                        dsdReader->setEof();
                                    }
                                } else {
                                    httpEof = true;
                                    dsdReader->setEof();
                                }
                            }

                            // === PHASE 2: Format detection ===
                            if (!formatLogged && dsdReader->isFormatReady()) {
                                formatLogged = true;
                                const auto& fmt = dsdReader->getFormat();
                                dsdBitRate = fmt.sampleRate;
                                detectedChannels = fmt.channels;
                                byteRateTotal = (static_cast<uint64_t>(dsdBitRate) / 8) * detectedChannels;

                                audioFmt.sampleRate = dsdBitRate;
                                audioFmt.bitDepth = 1;
                                audioFmt.channels = detectedChannels;
                                audioFmt.isDSD = true;
                                audioFmt.dsdFormat = (fmt.container == DsdFormat::Container::DFF)
                                    ? AudioFormat::DSDFormat::DFF
                                    : AudioFormat::DSDFormat::DSF;
                            }

                            // === PHASE 3: Prebuffer (wait for enough raw data) ===
                            if (formatLogged && !direttaOpened) {
                                size_t targetBytes = static_cast<size_t>(byteRateTotal * PREBUFFER_MS / 1000);
                                // Cap to achievable level: high DSD rates (DSD256/512)
                                // need more than DSD_BUF_MAX for 500ms, but flow control
                                // prevents the internal buffer from growing beyond DSD_BUF_MAX.
                                if (targetBytes > DSD_BUF_MAX * 3 / 4) {
                                    targetBytes = DSD_BUF_MAX * 3 / 4;
                                }

                                if (dsdReader->availableBytes() >= targetBytes || httpEof) {
                                    if (dsdReader->availableBytes() == 0) continue;

                                    if (!direttaPtr->open(audioFmt)) {
                                        LOG_ERROR("[Audio] Failed to open Diretta for DSD");
                                        slimproto->sendStat(StatEvent::STMn);
                                        audioThreadDone.store(true, std::memory_order_release);
                                        return;
                                    }

                                    uint32_t prebufMs = byteRateTotal > 0
                                        ? static_cast<uint32_t>(dsdReader->availableBytes() * 1000 / byteRateTotal) : 0;
                                    LOG_INFO("[Audio] DSD pre-buffered "
                                             << dsdReader->availableBytes()
                                             << " bytes (" << prebufMs << "ms)");

                                    // Flush prebuffer: readPlanar → sendAudio directly
                                    // Respect ring buffer capacity to avoid partial pushes
                                    while (audioTestRunning.load(std::memory_order_relaxed)) {
                                        if (direttaPtr->getBufferLevel() > 0.90f) break;
                                        size_t bytes = dsdReader->readPlanar(planarBuf, DSD_PLANAR_BUF);
                                        if (bytes == 0) break;
                                        size_t numSamples = (bytes * 8) / detectedChannels;
                                        direttaPtr->sendAudio(planarBuf, numSamples);
                                        pushedDsdBytes += bytes;
                                    }
                                    direttaOpened = true;
                                    slimproto->sendStat(StatEvent::STMl);
                                }
                                continue;
                            }

                            // === PHASE 4: Push DSD — readPlanar directly to sendAudio ===
                            if (direttaOpened && dsdReader->availableBytes() > 0) {
                                if (direttaPtr->isPaused()) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                } else if (direttaPtr->getBufferLevel() <= 0.95f) {
                                    size_t bytes = dsdReader->readPlanar(planarBuf, DSD_PLANAR_BUF);
                                    if (bytes > 0) {
                                        size_t numSamples = (bytes * 8) / detectedChannels;
                                        direttaPtr->sendAudio(planarBuf, numSamples);
                                        pushedDsdBytes += bytes;
                                    }
                                } else {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                            }

                            // === PHASE 5: Update elapsed time ===
                            if (direttaOpened && byteRateTotal > 0) {
                                uint64_t totalMs = (pushedDsdBytes * 1000) / byteRateTotal;
                                uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
                                uint32_t elapsedMs = static_cast<uint32_t>(totalMs);
                                slimproto->updateElapsed(elapsedSec, elapsedMs);

                                if (elapsedSec >= lastElapsedLog + 10) {
                                    lastElapsedLog = elapsedSec;
                                    LOG_DEBUG("[Audio] DSD elapsed: " << elapsedSec << "s"
                                              << " (" << pushedDsdBytes << " bytes pushed)"
                                              << " buf=" << dsdReader->availableBytes() << "b");
                                }
                            }

                            // === PHASE 6: Anti-busy-loop ===
                            if (!gotData && dsdReader->availableBytes() == 0 && !httpEof) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }

                            if (dsdReader->hasError()) {
                                LOG_ERROR("[Audio] DSD stream reader error");
                                break;
                            }
                        }

                        // === DRAIN remaining data ===
                        dsdReader->setEof();
                        while (direttaOpened &&
                               audioTestRunning.load(std::memory_order_acquire)) {
                            // Wait for DirettaSync space
                            while (audioTestRunning.load(std::memory_order_acquire)) {
                                if (direttaPtr->isPaused()) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                    continue;
                                }
                                if (direttaPtr->getBufferLevel() > 0.95f) {
                                    std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
                                    direttaPtr->waitForSpace(lock, std::chrono::milliseconds(5));
                                    continue;
                                }
                                break;
                            }
                            size_t bytes = dsdReader->readPlanar(planarBuf, DSD_PLANAR_BUF);
                            if (bytes == 0) break;
                            size_t numSamples = (bytes * 8) / detectedChannels;
                            direttaPtr->sendAudio(planarBuf, numSamples);
                            pushedDsdBytes += bytes;

                            if (byteRateTotal > 0) {
                                uint64_t totalMs = (pushedDsdBytes * 1000) / byteRateTotal;
                                slimproto->updateElapsed(
                                    static_cast<uint32_t>(totalMs / 1000),
                                    static_cast<uint32_t>(totalMs));
                            }
                        }

                        LOG_INFO("[Audio] DSD stream complete: " << totalBytes << " bytes received, "
                                 << pushedDsdBytes << " DSD bytes pushed");

                        slimproto->sendStat(StatEvent::STMd);
                        slimproto->sendStat(StatEvent::STMu);
                        audioThreadDone.store(true, std::memory_order_release);
                        return;
                    }

                    // ============================================================
                    // PCM/FLAC PATH
                    // ============================================================

                    // Create decoder for this format
                    auto decoder = Decoder::create(formatCode);
                    if (!decoder) {
                        LOG_ERROR("[Audio] Unsupported format: " << formatCode);
                        slimproto->sendStat(StatEvent::STMn);
                        audioThreadDone.store(true, std::memory_order_release);
                        return;
                    }

                    // Set raw PCM format hint from strm params (for Roon etc.)
                    if (formatCode == FORMAT_PCM) {
                        uint32_t sr = sampleRateFromCode(pcmRate);
                        uint32_t bd = sampleSizeFromCode(pcmSize);
                        uint32_t ch = (pcmChannels == '2') ? 2
                                    : (pcmChannels == '1') ? 1 : 0;
                        bool be = (pcmEndian == '0');
                        if (sr > 0 && bd > 0 && ch > 0) {
                            decoder->setRawPcmFormat(sr, bd, ch, be);
                        }
                    }

                    slimproto->sendStat(StatEvent::STMs);  // Stream started

                    uint8_t httpBuf[65536];
                    constexpr size_t MAX_DECODE_FRAMES = 1024;
                    int32_t decodeBuf[MAX_DECODE_FRAMES * 2];
                    uint64_t totalBytes = 0;
                    bool formatLogged = false;
                    uint64_t lastElapsedLog = 0;

                    // Decode cache: decouples HTTP reading from DirettaSync pushing.
                    // When DirettaSync buffer is full (flow control), we still read
                    // HTTP and decode into this cache. This prevents TCP starvation
                    // that caused underruns with bursty Qobuz streams.
                    // Max ~2s at 192kHz stereo = 768K samples
                    constexpr size_t DECODE_CACHE_MAX_SAMPLES = 768000;
                    std::vector<int32_t> decodeCache;
                    size_t decodeCachePos = 0;  // Read position (samples consumed)

                    constexpr unsigned int PREBUFFER_MS = 500;
                    uint64_t pushedFrames = 0;  // Frames actually sent to DirettaSync
                    bool direttaOpened = false;
                    AudioFormat audioFmt{};
                    int detectedChannels = 2;

                    // Helper: available frames in decode cache
                    auto cacheFrames = [&]() -> size_t {
                        return (decodeCache.size() - decodeCachePos) /
                               std::max(detectedChannels, 1);
                    };

                    bool httpEof = false;
                    while (audioTestRunning.load(std::memory_order_acquire) &&
                           (!httpEof || cacheFrames() > 0)) {

                        // ========== PHASE 1a: HTTP read ==========
                        // Read HTTP data and feed to decoder when cache has space.
                        bool gotData = false;
                        size_t cacheSamples = decodeCache.size() - decodeCachePos;
                        if (cacheSamples < DECODE_CACHE_MAX_SAMPLES && !httpEof) {
                            if (httpStream->isConnected()) {
                                ssize_t n = httpStream->readWithTimeout(
                                    httpBuf, sizeof(httpBuf), 2);
                                if (n > 0) {
                                    gotData = true;
                                    totalBytes += n;
                                    slimproto->updateStreamBytes(totalBytes);
                                    decoder->feed(httpBuf, static_cast<size_t>(n));
                                } else if (n < 0 || !httpStream->isConnected()) {
                                    httpEof = true;
                                    decoder->setEof();
                                }
                            } else {
                                httpEof = true;
                                decoder->setEof();
                            }
                        }

                        // ========== PHASE 1b: Drain decoder into cache ==========
                        // Always drain, even after httpEof — decoder may have
                        // buffered data from previous feed() calls.
                        if (decodeCache.size() - decodeCachePos <
                            DECODE_CACHE_MAX_SAMPLES) {
                            while (true) {
                                size_t frames = decoder->readDecoded(
                                    decodeBuf, MAX_DECODE_FRAMES);
                                if (frames == 0) break;
                                decodeCache.insert(decodeCache.end(), decodeBuf,
                                    decodeBuf + frames * detectedChannels);
                            }
                        }

                        // ========== PHASE 2: Format detection ==========
                        if (!formatLogged && decoder->isFormatReady()) {
                            formatLogged = true;
                            auto fmt = decoder->getFormat();
                            LOG_INFO("[Audio] Decoding: " << fmt.sampleRate << " Hz, "
                                     << fmt.bitDepth << "-bit, " << fmt.channels << " ch");
                            detectedChannels = fmt.channels;
                            audioFmt.sampleRate = fmt.sampleRate;
                            audioFmt.bitDepth = 32;
                            audioFmt.channels = fmt.channels;
                            audioFmt.isCompressed = (formatCode == 'f');
                        }

                        // ========== PHASE 3: Prebuffer phase ==========
                        if (formatLogged && !direttaOpened) {
                            auto fmt = decoder->getFormat();
                            size_t targetFrames = static_cast<size_t>(
                                fmt.sampleRate) * PREBUFFER_MS / 1000;
                            if (cacheFrames() >= targetFrames || httpEof) {
                                size_t prebufFrames = cacheFrames();
                                if (prebufFrames == 0) continue;

                                if (!direttaPtr->open(audioFmt)) {
                                    LOG_ERROR("[Audio] Failed to open Diretta output");
                                    slimproto->sendStat(StatEvent::STMn);
                                    audioThreadDone.store(true, std::memory_order_release);
                                    return;
                                }
                                // Set S24 pack mode hint AFTER open() — open() calls
                                // clear() which resets the hint. Our decoders always
                                // output MSB-aligned int32_t samples.
                                direttaPtr->setS24PackModeHint(
                                    DirettaRingBuffer::S24PackMode::MsbAligned);

                                uint32_t prebufMs = static_cast<uint32_t>(
                                    prebufFrames * 1000 / fmt.sampleRate);
                                LOG_INFO("[Audio] Pre-buffered " << prebufFrames
                                         << " frames (" << prebufMs << "ms)");

                                // Flush prebuffer at full speed (no flow control)
                                const int32_t* ptr = decodeCache.data() + decodeCachePos;
                                size_t remaining = prebufFrames;
                                while (remaining > 0 &&
                                       audioTestRunning.load(std::memory_order_relaxed)) {
                                    size_t chunk = std::min(remaining, MAX_DECODE_FRAMES);
                                    direttaPtr->sendAudio(
                                        reinterpret_cast<const uint8_t*>(ptr), chunk);
                                    ptr += chunk * detectedChannels;
                                    remaining -= chunk;
                                }
                                decodeCachePos += prebufFrames * detectedChannels;
                                pushedFrames += prebufFrames;
                                direttaOpened = true;
                                slimproto->sendStat(StatEvent::STMl);
                            }
                            continue;  // Stay in prebuffer mode
                        }

                        // ========== PHASE 4: Push from cache to DirettaSync ==========
                        if (direttaOpened && cacheFrames() > 0) {
                            if (direttaPtr->isPaused()) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(100));
                            } else if (direttaPtr->getBufferLevel() <= 0.95f) {
                                // Buffer has space - push one chunk
                                size_t push = std::min(cacheFrames(), MAX_DECODE_FRAMES);
                                direttaPtr->sendAudio(
                                    reinterpret_cast<const uint8_t*>(
                                        decodeCache.data() + decodeCachePos),
                                    push);
                                decodeCachePos += push * detectedChannels;
                                pushedFrames += push;
                            } else {
                                // Buffer full - sleep briefly, then loop back
                                // to read more HTTP (keeps TCP pipeline flowing)
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                            }
                        }

                        // ========== PHASE 5: Update elapsed time ==========
                        if (direttaOpened && decoder->isFormatReady()) {
                            auto fmt = decoder->getFormat();
                            if (fmt.sampleRate > 0) {
                                uint64_t totalMs = pushedFrames * 1000 / fmt.sampleRate;
                                uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
                                uint32_t elapsedMs = static_cast<uint32_t>(totalMs);
                                slimproto->updateElapsed(elapsedSec, elapsedMs);

                                if (elapsedSec >= lastElapsedLog + 10) {
                                    lastElapsedLog = elapsedSec;
                                    uint32_t totalSec = fmt.totalSamples > 0
                                        ? static_cast<uint32_t>(
                                            fmt.totalSamples / fmt.sampleRate) : 0;
                                    LOG_DEBUG("[Audio] Elapsed: " << elapsedSec << "s"
                                        << (totalSec > 0
                                            ? " / " + std::to_string(totalSec) + "s" : "")
                                        << " (" << pushedFrames << " pushed)"
                                        << " cache=" << cacheFrames() << "f");
                                }
                            }
                        }

                        // ========== PHASE 6: Compact cache ==========
                        // Periodically remove consumed samples to prevent
                        // unbounded growth of the vector
                        if (decodeCachePos > 100000) {
                            decodeCache.erase(decodeCache.begin(),
                                decodeCache.begin() + decodeCachePos);
                            decodeCachePos = 0;
                        }

                        // ========== PHASE 7: Anti-busy-loop ==========
                        if (!gotData && cacheFrames() == 0 && !httpEof) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }

                        if (decoder->hasError()) {
                            LOG_ERROR("[Audio] Decoder error");
                            break;
                        }
                    }

                    // Drain: decoder may have remaining frames after HTTP EOF
                    decoder->setEof();
                    while (!decoder->isFinished() && !decoder->hasError() &&
                           audioTestRunning.load(std::memory_order_acquire)) {
                        size_t frames = decoder->readDecoded(decodeBuf, MAX_DECODE_FRAMES);
                        if (frames == 0) break;
                        decodeCache.insert(decodeCache.end(), decodeBuf,
                            decodeBuf + frames * detectedChannels);
                    }

                    // Push remaining cache to DirettaSync
                    while (direttaOpened && cacheFrames() > 0 &&
                           audioTestRunning.load(std::memory_order_acquire)) {
                        while (audioTestRunning.load(std::memory_order_acquire)) {
                            if (direttaPtr->isPaused()) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(100));
                                continue;
                            }
                            if (direttaPtr->getBufferLevel() > 0.95f) {
                                std::unique_lock<std::mutex> lock(
                                    direttaPtr->getFlowMutex());
                                direttaPtr->waitForSpace(lock,
                                    std::chrono::milliseconds(5));
                                continue;
                            }
                            break;
                        }
                        size_t push = std::min(cacheFrames(), MAX_DECODE_FRAMES);
                        direttaPtr->sendAudio(
                            reinterpret_cast<const uint8_t*>(
                                decodeCache.data() + decodeCachePos),
                            push);
                        decodeCachePos += push * detectedChannels;
                        pushedFrames += push;

                        // Update elapsed during drain
                        if (decoder->isFormatReady()) {
                            auto fmt = decoder->getFormat();
                            if (fmt.sampleRate > 0) {
                                uint64_t totalMs = pushedFrames * 1000 / fmt.sampleRate;
                                uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
                                uint32_t elapsedMs = static_cast<uint32_t>(totalMs);
                                slimproto->updateElapsed(elapsedSec, elapsedMs);
                            }
                        }
                    }

                    // Final elapsed time
                    if (decoder->isFormatReady()) {
                        auto fmt = decoder->getFormat();
                        uint64_t decoded = decoder->getDecodedSamples();
                        uint32_t elapsedSec = fmt.sampleRate > 0
                            ? static_cast<uint32_t>(decoded / fmt.sampleRate) : 0;
                        LOG_INFO("[Audio] Stream complete: " << totalBytes << " bytes received, "
                                 << decoded << " frames decoded (" << elapsedSec << "s)");
                    } else {
                        LOG_INFO("[Audio] Stream ended (" << totalBytes << " bytes received)");
                    }

                    slimproto->sendStat(StatEvent::STMd);  // Decoder finished
                    slimproto->sendStat(StatEvent::STMu);  // Underrun (natural end)
                    audioThreadDone.store(true, std::memory_order_release);
                });
                break;
            }

            case STRM_STOP:
                LOG_INFO("Stream stop requested");
                audioTestRunning.store(false);
                httpStream->disconnect();
                if (direttaPtr->isPlaying()) direttaPtr->stopPlayback(true);
                slimproto->sendStat(StatEvent::STMf);  // Flushed
                break;

            case STRM_PAUSE:
                LOG_INFO("Pause requested");
                direttaPtr->pausePlayback();
                slimproto->sendStat(StatEvent::STMp);
                break;

            case STRM_UNPAUSE:
                LOG_INFO("Unpause requested");
                direttaPtr->resumePlayback();
                slimproto->sendStat(StatEvent::STMr);
                break;

            case STRM_FLUSH:
                LOG_INFO("Flush requested");
                audioTestRunning.store(false);
                httpStream->disconnect();
                if (direttaPtr->isPlaying()) direttaPtr->stopPlayback(true);
                slimproto->sendStat(StatEvent::STMf);
                break;

            default:
                break;
        }
    });

    slimproto->onVolume([](uint32_t gainL, uint32_t gainR) {
        LOG_DEBUG("Volume: L=0x" << std::hex << gainL << " R=0x" << gainR
                  << std::dec << " (ignored - bit-perfect)");
    });

    // Helper: stop audio thread and wait for it to finish
    auto stopAudioThread = [&]() {
        audioTestRunning.store(false);
        httpStream->disconnect();
        if (audioTestThread.joinable()) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!audioThreadDone.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (audioThreadDone.load(std::memory_order_acquire)) {
                audioTestThread.join();
            } else {
                audioTestThread.detach();
                LOG_WARN("Audio thread did not stop in time, detached");
            }
        }
        if (direttaPtr->isPlaying()) direttaPtr->stopPlayback(true);
    };

    // Helper: interruptible sleep (returns false if shutdown requested)
    auto interruptibleSleep = [](int seconds) -> bool {
        for (int i = 0; i < seconds * 10; i++) {
            if (!g_running.load(std::memory_order_acquire)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };

    // ============================================
    // Connection loop with exponential backoff
    // ============================================

    constexpr int INITIAL_BACKOFF_S = 2;
    constexpr int MAX_BACKOFF_S = 30;
    int backoffS = INITIAL_BACKOFF_S;
    int connectionCount = 0;

    while (g_running.load(std::memory_order_acquire)) {
        // Wait before reconnection (skip on first attempt)
        if (connectionCount > 0) {
            LOG_WARN("Reconnecting to LMS in " << backoffS << "s...");
            if (!interruptibleSleep(backoffS)) break;
            backoffS = std::min(backoffS * 2, MAX_BACKOFF_S);
        }

        // Connect to LMS
        if (!slimproto->connect(config.lmsServer, config.lmsPort, config)) {
            if (g_running.load(std::memory_order_acquire)) {
                LOG_WARN("Failed to connect to LMS");
                // Start backoff even on first attempt failure
                if (connectionCount == 0) connectionCount = 1;
            }
            continue;
        }

        // Success — reset backoff
        backoffS = INITIAL_BACKOFF_S;
        connectionCount++;

        // Run slimproto receive loop in a dedicated thread
        std::thread slimprotoThread([&slimproto]() {
            slimproto->run();
        });

        if (connectionCount == 1) {
            LOG_INFO("Player registered with LMS");
            std::cout << "(Press Ctrl+C to stop)" << std::endl;
        } else {
            LOG_INFO("Reconnected to LMS");
        }
        std::cout << std::endl;

        // Wait for shutdown signal or connection loss
        while (g_running.load(std::memory_order_acquire) && slimproto->isConnected()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Cleanup: stop audio, disconnect slimproto, join thread
        stopAudioThread();
        slimproto->disconnect();
        if (slimprotoThread.joinable()) {
            slimprotoThread.join();
        }

        if (!g_running.load(std::memory_order_acquire)) break;
        LOG_WARN("Lost connection to LMS");
    }

    // ============================================
    // Final shutdown
    // ============================================

    std::cout << "\nShutting down..." << std::endl;
    stopAudioThread();
    g_slimproto = nullptr;
    slimproto->disconnect();

    if (diretta->isOpen()) diretta->close();
    diretta->disable();
    g_diretta = nullptr;

    shutdownAsyncLogging();
    return 0;
}
