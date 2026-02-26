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

    // SDK warm-up: open at 48kHz (most common Qobuz rate), push silence
    // to exercise the full data path (ring buffer -> worker -> network -> target),
    // then stopPlayback to leave the connection alive.
    //
    // Why not close()? close() calls disconnect(true) but leaves m_sdkOpen=true,
    // causing the next open()'s setSink to fail on the stale connection.
    //
    // With stopPlayback: connection stays alive, m_open=true, m_hasPreviousFormat=true.
    // First real track at 48kHz -> quick resume (0 underruns).
    // First real track at other rate -> format change path (proper close/reopen).
    {
        AudioFormat warmupFmt;
        warmupFmt.sampleRate = 48000;
        warmupFmt.bitDepth = 32;
        warmupFmt.channels = 2;
        warmupFmt.isCompressed = false;
        diretta->setS24PackModeHint(DirettaRingBuffer::S24PackMode::MsbAligned);
        if (diretta->open(warmupFmt)) {
            // Push silence to fill prefill buffer and start data flow to target
            constexpr size_t SILENCE_FRAMES = 1024;
            int32_t silence[SILENCE_FRAMES * 2] = {};  // Zero-initialized
            for (int i = 0; i < 12; i++) {  // ~12K frames = 250ms at 48kHz
                diretta->sendAudio(
                    reinterpret_cast<const uint8_t*>(silence), SILENCE_FRAMES);
            }
            // Let data flow to target to complete the full init path
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            diretta->stopPlayback(true);
            LOG_INFO("SDK warm-up complete (48000/32/2, data path exercised)");
        } else {
            LOG_WARN("SDK warm-up open() failed — first track may have underruns");
        }
    }

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
                audioTestRunning.store(true);
                audioThreadDone.store(false, std::memory_order_release);
                audioTestThread = std::thread([&httpStream, &slimproto, &audioTestRunning, &audioThreadDone, formatCode, direttaPtr]() {
                    // Create decoder for this format
                    auto decoder = Decoder::create(formatCode);
                    if (!decoder) {
                        LOG_ERROR("[Audio] Unsupported format: " << formatCode);
                        slimproto->sendStat(StatEvent::STMn);
                        audioThreadDone.store(true, std::memory_order_release);
                        return;
                    }

                    slimproto->sendStat(StatEvent::STMs);  // Stream started

                    uint8_t httpBuf[65536];
                    // Decode buffer: up to 1024 frames * 2 channels
                    constexpr size_t MAX_DECODE_FRAMES = 1024;
                    int32_t decodeBuf[MAX_DECODE_FRAMES * 2];
                    uint64_t totalBytes = 0;
                    bool formatLogged = false;
                    uint64_t lastElapsedLog = 0;

                    // Pre-buffer: accumulate 500ms of decoded audio before
                    // opening DirettaSync to prevent initial underruns
                    constexpr unsigned int PREBUFFER_MS = 500;
                    std::vector<int32_t> prebuffer;
                    size_t prebufferFrames = 0;
                    bool direttaOpened = false;
                    AudioFormat audioFmt{};
                    int detectedChannels = 2;

                    // Read HTTP -> feed decoder -> read decoded frames
                    // Uses readWithTimeout to avoid blocking on recv() which
                    // would starve the ring buffer during network waits.
                    bool httpEof = false;
                    while (audioTestRunning.load(std::memory_order_acquire) &&
                           !httpEof) {
                        // Non-blocking HTTP read (2ms timeout)
                        // Short timeout keeps ring buffer fed even during
                        // network stalls at high sample rates (768kHz+ = 6MB/s)
                        bool gotData = false;
                        if (httpStream->isConnected()) {
                            ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 2);
                            if (n > 0) {
                                gotData = true;
                                totalBytes += n;
                                slimproto->updateStreamBytes(totalBytes);
                                decoder->feed(httpBuf, static_cast<size_t>(n));
                            } else if (n < 0 || !httpStream->isConnected()) {
                                // Real error or EOF
                                httpEof = true;
                                decoder->setEof();
                            }
                            // n == 0: timeout, continue to drain decoder buffer
                        } else {
                            httpEof = true;
                            decoder->setEof();
                        }

                        // Pull decoded frames (from new data or decoder's internal buffer)
                        bool decodedAny = false;
                        while (audioTestRunning.load(std::memory_order_relaxed)) {
                            size_t frames = decoder->readDecoded(decodeBuf, MAX_DECODE_FRAMES);
                            if (frames == 0) break;
                            decodedAny = true;

                            // Detect format on first successful decode
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

                                // Reserve for pre-buffer
                                size_t targetFrames = static_cast<size_t>(
                                    fmt.sampleRate) * PREBUFFER_MS / 1000;
                                prebuffer.reserve(targetFrames * fmt.channels);
                            }

                            // Pre-buffer phase: accumulate frames before opening
                            if (formatLogged && !direttaOpened) {
                                prebuffer.insert(prebuffer.end(), decodeBuf,
                                                 decodeBuf + frames * detectedChannels);
                                prebufferFrames += frames;

                                auto fmt = decoder->getFormat();
                                size_t targetFrames = static_cast<size_t>(
                                    fmt.sampleRate) * PREBUFFER_MS / 1000;
                                if (prebufferFrames >= targetFrames) {
                                    // Open DirettaSync and flush pre-buffer
                                    direttaPtr->setS24PackModeHint(
                                        DirettaRingBuffer::S24PackMode::MsbAligned);
                                    if (!direttaPtr->open(audioFmt)) {
                                        LOG_ERROR("[Audio] Failed to open Diretta output");
                                        slimproto->sendStat(StatEvent::STMn);
                                        audioThreadDone.store(true, std::memory_order_release);
                                        return;
                                    }

                                    uint32_t prebufMs = static_cast<uint32_t>(
                                        prebufferFrames * 1000 / fmt.sampleRate);
                                    LOG_INFO("[Audio] Pre-buffered " << prebufferFrames
                                             << " frames (" << prebufMs << "ms)");

                                    // Push prebuffer at full speed (no flow control)
                                    // Buffer starts empty after stopPlayback, so no
                                    // risk of overflow during initial fill
                                    const int32_t* ptr = prebuffer.data();
                                    size_t remaining = prebufferFrames;
                                    while (remaining > 0 &&
                                           audioTestRunning.load(std::memory_order_relaxed)) {
                                        size_t chunk = std::min(remaining, MAX_DECODE_FRAMES);
                                        direttaPtr->sendAudio(
                                            reinterpret_cast<const uint8_t*>(ptr), chunk);
                                        ptr += chunk * detectedChannels;
                                        remaining -= chunk;
                                    }

                                    prebuffer.clear();
                                    prebuffer.shrink_to_fit();
                                    direttaOpened = true;
                                    slimproto->sendStat(StatEvent::STMl);
                                }
                                continue;  // Stay in prebuffer mode
                            }

                            // Update elapsed time for LMS progress bar
                            if (decoder->isFormatReady()) {
                                auto fmt = decoder->getFormat();
                                if (fmt.sampleRate > 0) {
                                    uint64_t decoded = decoder->getDecodedSamples();
                                    uint32_t elapsedSec = static_cast<uint32_t>(decoded / fmt.sampleRate);
                                    uint32_t elapsedMs = static_cast<uint32_t>(
                                        (decoded % fmt.sampleRate) * 1000 / fmt.sampleRate);
                                    slimproto->updateElapsed(elapsedSec, elapsedMs);

                                    if (elapsedSec >= lastElapsedLog + 10) {
                                        lastElapsedLog = elapsedSec;
                                        uint32_t totalSec = fmt.totalSamples > 0
                                            ? static_cast<uint32_t>(fmt.totalSamples / fmt.sampleRate) : 0;
                                        LOG_DEBUG("[Audio] Elapsed: " << elapsedSec << "s"
                                                  << (totalSec > 0 ? " / " + std::to_string(totalSec) + "s" : "")
                                                  << " (" << decoded << " frames)");
                                    }
                                }
                            }

                            // Flow control: wait when paused or buffer near full
                            // Threshold 0.95 with short 5ms wait to avoid
                            // throttling throughput below consumption rate
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

                            direttaPtr->sendAudio(
                                reinterpret_cast<const uint8_t*>(decodeBuf),
                                frames);
                        }

                        // If no HTTP data and no decoded data, avoid busy-loop
                        if (!gotData && !decodedAny && !httpEof) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }

                        if (decoder->hasError()) {
                            LOG_ERROR("[Audio] Decoder error");
                            break;
                        }
                    }

                    // If stream ended during pre-buffer (short track), flush what we have
                    if (formatLogged && !direttaOpened && prebufferFrames > 0 &&
                        audioTestRunning.load(std::memory_order_acquire)) {
                        direttaPtr->setS24PackModeHint(
                            DirettaRingBuffer::S24PackMode::MsbAligned);
                        if (direttaPtr->open(audioFmt)) {
                            auto fmt = decoder->getFormat();
                            uint32_t prebufMs = static_cast<uint32_t>(
                                prebufferFrames * 1000 / fmt.sampleRate);
                            LOG_INFO("[Audio] Short track pre-buffer: " << prebufferFrames
                                     << " frames (" << prebufMs << "ms)");

                            const int32_t* ptr = prebuffer.data();
                            size_t remaining = prebufferFrames;
                            while (remaining > 0) {
                                size_t chunk = std::min(remaining, MAX_DECODE_FRAMES);
                                direttaPtr->sendAudio(
                                    reinterpret_cast<const uint8_t*>(ptr), chunk);
                                ptr += chunk * detectedChannels;
                                remaining -= chunk;
                            }
                            direttaOpened = true;
                            slimproto->sendStat(StatEvent::STMl);
                        }
                        prebuffer.clear();
                        prebuffer.shrink_to_fit();
                    }

                    // Drain remaining decoded frames after HTTP stream ends
                    decoder->setEof();
                    while (!decoder->isFinished() && !decoder->hasError() &&
                           audioTestRunning.load(std::memory_order_acquire)) {
                        size_t frames = decoder->readDecoded(decodeBuf, MAX_DECODE_FRAMES);
                        if (frames == 0) break;

                        if (direttaOpened) {
                            // Flow control: wait when paused or buffer near full
                            // Threshold 0.95 with short 5ms wait to avoid
                            // throttling throughput below consumption rate
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

                            direttaPtr->sendAudio(
                                reinterpret_cast<const uint8_t*>(decodeBuf),
                                frames);
                        }

                        // Update elapsed during drain
                        if (decoder->isFormatReady()) {
                            auto fmt = decoder->getFormat();
                            if (fmt.sampleRate > 0) {
                                uint64_t decoded = decoder->getDecodedSamples();
                                uint32_t elapsedSec = static_cast<uint32_t>(decoded / fmt.sampleRate);
                                uint32_t elapsedMs = static_cast<uint32_t>(
                                    (decoded % fmt.sampleRate) * 1000 / fmt.sampleRate);
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

    if (!slimproto->connect(config.lmsServer, config.lmsPort, config)) {
        std::cerr << "Failed to connect to LMS" << std::endl;
        diretta->disable();
        g_diretta = nullptr;
        shutdownAsyncLogging();
        return 1;
    }

    // Run slimproto receive loop in a dedicated thread
    std::thread slimprotoThread([&slimproto]() {
        slimproto->run();
    });

    std::cout << "Player registered with LMS" << std::endl;
    std::cout << "(Press Ctrl+C to stop)" << std::endl;
    std::cout << std::endl;

    // Wait for shutdown signal
    while (g_running.load(std::memory_order_acquire) && slimproto->isConnected()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Clean shutdown
    std::cout << "\nShutting down..." << std::endl;
    audioTestRunning.store(false);
    httpStream->disconnect();
    if (audioTestThread.joinable()) {
        // Wait up to 1s for the audio thread to finish
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!audioThreadDone.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (audioThreadDone.load(std::memory_order_acquire)) {
            audioTestThread.join();
        } else {
            audioTestThread.detach();
        }
    }
    // Shutdown DirettaSync
    if (diretta->isOpen()) diretta->close();
    diretta->disable();
    g_diretta = nullptr;

    g_slimproto = nullptr;
    slimproto->disconnect();
    if (slimprotoThread.joinable()) {
        slimprotoThread.join();
    }

    shutdownAsyncLogging();
    return 0;
}
