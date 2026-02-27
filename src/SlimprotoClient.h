/**
 * @file SlimprotoClient.h
 * @brief Slimproto TCP protocol client for LMS communication
 *
 * Implements the SlimProto binary protocol to register as a player
 * with Lyrion Music Server and receive streaming commands.
 *
 * Protocol reference: wiki.lyrion.org (public), Rust slimproto crate (MIT)
 */

#ifndef SLIM2DIRETTA_SLIMPROTO_CLIENT_H
#define SLIM2DIRETTA_SLIMPROTO_CLIENT_H

#include "SlimprotoMessages.h"
#include "Config.h"

#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <chrono>

class SlimprotoClient {
public:
    // Callback types
    using StreamCallback = std::function<void(const StrmCommand& cmd, const std::string& httpRequest)>;
    using VolumeCallback = std::function<void(uint32_t gainLeft, uint32_t gainRight)>;

    SlimprotoClient();
    ~SlimprotoClient();

    // Non-copyable
    SlimprotoClient(const SlimprotoClient&) = delete;
    SlimprotoClient& operator=(const SlimprotoClient&) = delete;

    // Lifecycle
    bool connect(const std::string& server, uint16_t port, const Config& config);
    void disconnect();
    bool isConnected() const;

    // Run receive loop (blocks until disconnect/error - call from slimproto thread)
    void run();
    void stop();

    // Register callbacks (set before calling run())
    void onStream(StreamCallback cb) { m_streamCb = std::move(cb); }
    void onVolume(VolumeCallback cb) { m_volumeCb = std::move(cb); }

    // Send status to server (thread-safe)
    void sendStat(const char eventCode[4], uint32_t serverTimestamp = 0);

    // Send HTTP response headers back to server
    void sendResp(const std::string& headers);

    // Update state for STAT messages (called from audio thread)
    void updateStreamBytes(uint64_t bytes);
    void updateElapsed(uint32_t seconds, uint32_t milliseconds);
    void updateBufferState(uint32_t streamBufSize, uint32_t streamBufFull,
                           uint32_t outputBufSize, uint32_t outputBufFull);

    // Get the server IP used for the control connection
    const std::string& getServerIp() const { return m_serverIp; }

private:
    int m_socket = -1;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::mutex m_sendMutex;  // Protects socket writes
    std::string m_serverIp;

    Config m_config;
    uint8_t m_mac[6] = {};

    // Callbacks
    StreamCallback m_streamCb;
    VolumeCallback m_volumeCb;

    // State for STAT messages (atomics for cross-thread access)
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint32_t> m_elapsedSeconds{0};
    std::atomic<uint32_t> m_elapsedMs{0};
    std::atomic<uint32_t> m_streamBufSize{0};
    std::atomic<uint32_t> m_streamBufFull{0};
    std::atomic<uint32_t> m_outputBufSize{0};
    std::atomic<uint32_t> m_outputBufFull{0};
    uint32_t m_serverTimestamp = 0;

    // Startup time for jiffies calculation
    std::chrono::steady_clock::time_point m_startTime;

    // Internal protocol methods
    void sendHelo();
    void sendBye();
    void sendSetd(uint8_t id, const std::string& data);

    // Server message handlers
    void processServerMessage(const char opcode[4], const uint8_t* data, size_t len);
    void handleStrm(const uint8_t* data, size_t len);
    void handleAudg(const uint8_t* data, size_t len);
    void handleSetd(const uint8_t* data, size_t len);

    // Socket I/O helpers
    bool readExact(void* buf, size_t len);
    bool sendAll(const void* buf, size_t len);
    bool sendMessage(const char opcode[4], const void* payload, size_t payloadLen);

    // MAC address
    void generateMac();
    bool parseMac(const std::string& str);

    // Jiffies (ms since startup)
    uint32_t getJiffies() const;

    // Capabilities string
    std::string buildCapabilities() const;
};

#endif // SLIM2DIRETTA_SLIMPROTO_CLIENT_H
