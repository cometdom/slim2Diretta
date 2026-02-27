/**
 * @file SlimprotoClient.cpp
 * @brief Slimproto TCP protocol client implementation
 *
 * Clean-room implementation from public protocol documentation.
 * Reference: wiki.lyrion.org, Rust slimproto crate (MIT)
 */

#include "SlimprotoClient.h"
#include "LogLevel.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <random>

// ============================================
// Constructor / Destructor
// ============================================

SlimprotoClient::SlimprotoClient()
    : m_startTime(std::chrono::steady_clock::now())
{
}

SlimprotoClient::~SlimprotoClient() {
    disconnect();
}

// ============================================
// Connection Management
// ============================================

bool SlimprotoClient::connect(const std::string& server, uint16_t port, const Config& config) {
    m_config = config;
    m_serverIp = server;

    // Parse or generate MAC address
    if (!config.macAddress.empty()) {
        if (!parseMac(config.macAddress)) {
            LOG_ERROR("Invalid MAC address format: " << config.macAddress);
            return false;
        }
    } else {
        generateMac();
    }

    // Log MAC
    std::ostringstream macStr;
    macStr << std::hex << std::setfill('0');
    for (int i = 0; i < 6; i++) {
        if (i > 0) macStr << ":";
        macStr << std::setw(2) << static_cast<int>(m_mac[i]);
    }
    LOG_INFO("Player MAC: " << macStr.str());

    // Create TCP socket
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("Failed to create socket: " << strerror(errno));
        return false;
    }

    // Set TCP_NODELAY for low-latency control messages
    int flag = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Connect to LMS
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("Invalid server address: " << server);
        close(m_socket);
        m_socket = -1;
        return false;
    }

    LOG_INFO("Connecting to LMS at " << server << ":" << port << "...");

    if (::connect(m_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to connect to LMS: " << strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_connected.store(true, std::memory_order_release);
    LOG_INFO("Connected to LMS");

    // Send HELO to register as a player
    sendHelo();

    // Send player name to LMS (setd id=0)
    sendSetd(0, m_config.playerName);

    return true;
}

void SlimprotoClient::disconnect() {
    if (m_connected.load(std::memory_order_acquire)) {
        sendBye();
        m_connected.store(false, std::memory_order_release);
    }

    m_running.store(false, std::memory_order_release);

    if (m_socket >= 0) {
        // Shutdown the socket to unblock any pending read
        shutdown(m_socket, SHUT_RDWR);
        close(m_socket);
        m_socket = -1;
    }
}

bool SlimprotoClient::isConnected() const {
    return m_connected.load(std::memory_order_acquire);
}

// ============================================
// Receive Loop
// ============================================

void SlimprotoClient::run() {
    m_running.store(true, std::memory_order_release);

    LOG_DEBUG("[Slimproto] Receive loop started");

    while (m_running.load(std::memory_order_acquire)) {
        // Server -> Client frame: [2-byte length BE][4-byte opcode][payload]
        uint16_t frameLen = 0;
        if (!readExact(&frameLen, 2)) {
            if (m_running.load(std::memory_order_acquire)) {
                LOG_WARN("Lost connection to LMS");
            }
            break;
        }
        frameLen = ntohs(frameLen);

        if (frameLen < 4) {
            LOG_WARN("[Slimproto] Invalid frame length: " << frameLen);
            continue;
        }

        // Read opcode
        char opcode[4];
        if (!readExact(opcode, 4)) {
            break;
        }

        // Read payload
        size_t payloadLen = frameLen - 4;
        std::vector<uint8_t> payload(payloadLen);
        if (payloadLen > 0) {
            if (!readExact(payload.data(), payloadLen)) {
                break;
            }
        }

        processServerMessage(opcode, payload.data(), payloadLen);
    }

    LOG_DEBUG("[Slimproto] Receive loop ended");
    m_connected.store(false, std::memory_order_release);
}

void SlimprotoClient::stop() {
    m_running.store(false, std::memory_order_release);
    // Shutdown socket to unblock readExact
    if (m_socket >= 0) {
        shutdown(m_socket, SHUT_RDWR);
    }
}

// ============================================
// Server Message Dispatch
// ============================================

void SlimprotoClient::processServerMessage(const char opcode[4], const uint8_t* data, size_t len) {
    if (std::memcmp(opcode, "strm", 4) == 0) {
        handleStrm(data, len);
    }
    else if (std::memcmp(opcode, "audg", 4) == 0) {
        handleAudg(data, len);
    }
    else if (std::memcmp(opcode, "setd", 4) == 0) {
        handleSetd(data, len);
    }
    else if (std::memcmp(opcode, "serv", 4) == 0) {
        // Server redirect - for now just log it
        if (len >= 4) {
            struct in_addr addr;
            std::memcpy(&addr, data, 4);
            LOG_INFO("[Slimproto] Server redirect to " << inet_ntoa(addr));
        }
    }
    else if (std::memcmp(opcode, "vers", 4) == 0) {
        std::string version(reinterpret_cast<const char*>(data), len);
        LOG_INFO("LMS version: " << version);
    }
    else if (std::memcmp(opcode, "aude", 4) == 0) {
        // Audio enable - just acknowledge
        LOG_DEBUG("[Slimproto] aude received (audio enable)");
    }
    else if (std::memcmp(opcode, "vfdc", 4) == 0 ||
             std::memcmp(opcode, "grfe", 4) == 0 ||
             std::memcmp(opcode, "grfb", 4) == 0) {
        // Visualization/display commands - silently ignore (no screen)
    }
    else {
        // Unknown command - log and skip
        std::string cmd(opcode, 4);
        LOG_DEBUG("[Slimproto] Unknown command: " << cmd << " (" << len << " bytes)");
    }
}

// ============================================
// strm Handler
// ============================================

void SlimprotoClient::handleStrm(const uint8_t* data, size_t len) {
    if (len < sizeof(StrmCommand)) {
        LOG_WARN("[Slimproto] strm too short: " << len << " bytes");
        return;
    }

    StrmCommand cmd;
    std::memcpy(&cmd, data, sizeof(StrmCommand));

    // Extract HTTP request string (after the 24-byte fixed header)
    std::string httpRequest;
    if (len > sizeof(StrmCommand)) {
        httpRequest.assign(reinterpret_cast<const char*>(data + sizeof(StrmCommand)),
                           len - sizeof(StrmCommand));
    }

    switch (cmd.command) {
        case STRM_START:
            LOG_INFO("[Slimproto] strm-s: format=" << cmd.format
                     << " rate=" << cmd.pcmSampleRate
                     << " size=" << cmd.pcmSampleSize
                     << " ch=" << cmd.pcmChannels
                     << " port=" << cmd.getServerPort());
            LOG_DEBUG("[Slimproto] HTTP request: " << httpRequest.substr(0, 120));
            break;

        case STRM_STOP:
            LOG_INFO("[Slimproto] strm-q: stop");
            break;

        case STRM_PAUSE: {
            uint32_t interval = cmd.getReplayGain();
            if (interval > 0) {
                LOG_INFO("[Slimproto] strm-p: pause for " << interval << " ms");
            } else {
                LOG_INFO("[Slimproto] strm-p: pause");
            }
            break;
        }

        case STRM_UNPAUSE:
            LOG_INFO("[Slimproto] strm-u: unpause");
            break;

        case STRM_FLUSH:
            LOG_INFO("[Slimproto] strm-f: flush");
            break;

        case STRM_STATUS: {
            // Heartbeat - respond with STMt
            uint32_t ts = cmd.getReplayGain();
            m_serverTimestamp = ts;
            sendStat(StatEvent::STMt, ts);
            // Log heartbeat only once per minute to reduce noise
            {
                static uint32_t lastLoggedTs = 0;
                if (ts == 0 || ts >= lastLoggedTs + 60000) {
                    LOG_DEBUG("[Slimproto] heartbeat (ts="
                              << ts << ")");
                    lastLoggedTs = ts;
                }
            }
            return;  // Don't invoke stream callback for heartbeats
        }

        case STRM_SKIP:
            LOG_INFO("[Slimproto] strm-a: skip");
            break;

        default:
            LOG_WARN("[Slimproto] Unknown strm command: " << cmd.command);
            return;
    }

    // Invoke callback
    if (m_streamCb) {
        m_streamCb(cmd, httpRequest);
    }
}

// ============================================
// audg Handler
// ============================================

void SlimprotoClient::handleAudg(const uint8_t* data, size_t len) {
    if (len < sizeof(AudgCommand)) {
        LOG_WARN("[Slimproto] audg too short: " << len << " bytes");
        return;
    }

    AudgCommand cmd;
    std::memcpy(&cmd, data, sizeof(AudgCommand));

    uint32_t gainL = cmd.getNewGainLeft();
    uint32_t gainR = cmd.getNewGainRight();

    // Log but force 100% volume for bit-perfect
    LOG_DEBUG("[Slimproto] audg: gainL=0x" << std::hex << gainL
             << " gainR=0x" << gainR << std::dec
             << " (ignored - bit-perfect mode)");

    if (m_volumeCb) {
        m_volumeCb(gainL, gainR);
    }
}

// ============================================
// setd Handler
// ============================================

void SlimprotoClient::handleSetd(const uint8_t* data, size_t len) {
    if (len < 1) return;

    uint8_t id = data[0];
    if (id == 0 && len > 1) {
        // LMS sets player name
        std::string name(reinterpret_cast<const char*>(data + 1), len - 1);
        while (!name.empty() && name.back() == '\0') {
            name.pop_back();
        }
        LOG_INFO("[Slimproto] Player name set to: " << name);
    } else if (id == 0 && len == 1) {
        // LMS queries player name — respond with configured name
        sendSetd(0, m_config.playerName);
    } else {
        LOG_DEBUG("[Slimproto] setd id=" << static_cast<int>(id) << " (" << len - 1 << " bytes)");
    }
}

// ============================================
// Send HELO
// ============================================

void SlimprotoClient::sendHelo() {
    std::string caps = buildCapabilities();

    // Build payload
    std::vector<uint8_t> payload(sizeof(HeloPayload) + caps.size());

    HeloPayload helo{};
    helo.deviceId = DEVICE_ID_SQUEEZEPLAY;
    helo.revision = 0;
    std::memcpy(helo.mac, m_mac, 6);
    std::memset(helo.uuid, 0, 16);
    helo.wlanChannels = 0;
    helo.bytesRecvHi = 0;
    helo.bytesRecvLo = 0;
    helo.language[0] = 'e';
    helo.language[1] = 'n';

    std::memcpy(payload.data(), &helo, sizeof(HeloPayload));
    std::memcpy(payload.data() + sizeof(HeloPayload), caps.c_str(), caps.size());

    sendMessage("HELO", payload.data(), payload.size());

    LOG_INFO("HELO sent (capabilities: " << caps << ")");
}

// ============================================
// Send BYE
// ============================================

void SlimprotoClient::sendBye() {
    uint8_t reason = 0;  // Normal disconnect
    sendMessage("BYE!", &reason, 1);
    LOG_DEBUG("[Slimproto] BYE sent");
}

// ============================================
// Send setd (device setting to server)
// ============================================

void SlimprotoClient::sendSetd(uint8_t id, const std::string& data) {
    std::vector<uint8_t> payload(1 + data.size());
    payload[0] = id;
    if (!data.empty()) {
        std::memcpy(payload.data() + 1, data.c_str(), data.size());
    }
    sendMessage("SETD", payload.data(), payload.size());
    LOG_DEBUG("[Slimproto] setd sent: id=" << static_cast<int>(id)
             << " data=\"" << data << "\"");
}

// ============================================
// Send STAT
// ============================================

void SlimprotoClient::sendStat(const char eventCode[4], uint32_t serverTimestamp) {
    StatPayload stat{};

    std::memcpy(stat.eventCode, eventCode, 4);
    stat.crlf = 0;
    stat.masInit = 0;
    stat.masMode = 0;
    stat.streamBufSize = htonl(m_streamBufSize.load(std::memory_order_relaxed));
    stat.streamBufFull = htonl(m_streamBufFull.load(std::memory_order_relaxed));

    uint64_t bytes = m_bytesReceived.load(std::memory_order_relaxed);
    stat.bytesRecvHi = htonl(static_cast<uint32_t>(bytes >> 32));
    stat.bytesRecvLo = htonl(static_cast<uint32_t>(bytes & 0xFFFFFFFF));

    stat.signalStrength = htons(0xFFFF);  // Wired connection
    stat.jiffies = htonl(getJiffies());
    stat.outputBufSize = htonl(m_outputBufSize.load(std::memory_order_relaxed));
    stat.outputBufFull = htonl(m_outputBufFull.load(std::memory_order_relaxed));
    stat.elapsedSeconds = htonl(m_elapsedSeconds.load(std::memory_order_relaxed));
    stat.voltage = 0;
    stat.elapsedMs = htonl(m_elapsedMs.load(std::memory_order_relaxed));
    stat.serverTimestamp = htonl(serverTimestamp);
    stat.errorCode = 0;

    sendMessage("STAT", &stat, sizeof(stat));

    // Don't log heartbeat responses (too noisy — every 2s)
    if (std::memcmp(eventCode, StatEvent::STMt, 4) != 0) {
        std::string evt(eventCode, 4);
        LOG_DEBUG("[Slimproto] STAT sent: " << evt);
    }
}

// ============================================
// Send RESP (HTTP response headers)
// ============================================

void SlimprotoClient::sendResp(const std::string& headers) {
    sendMessage("RESP", headers.c_str(), headers.size());
    LOG_DEBUG("[Slimproto] RESP sent (" << headers.size() << " bytes)");
}

// ============================================
// State Updates (from audio thread)
// ============================================

void SlimprotoClient::updateStreamBytes(uint64_t bytes) {
    m_bytesReceived.store(bytes, std::memory_order_relaxed);
}

void SlimprotoClient::updateElapsed(uint32_t seconds, uint32_t milliseconds) {
    m_elapsedSeconds.store(seconds, std::memory_order_relaxed);
    m_elapsedMs.store(milliseconds, std::memory_order_relaxed);
}

void SlimprotoClient::updateBufferState(uint32_t streamBufSize, uint32_t streamBufFull,
                                         uint32_t outputBufSize, uint32_t outputBufFull) {
    m_streamBufSize.store(streamBufSize, std::memory_order_relaxed);
    m_streamBufFull.store(streamBufFull, std::memory_order_relaxed);
    m_outputBufSize.store(outputBufSize, std::memory_order_relaxed);
    m_outputBufFull.store(outputBufFull, std::memory_order_relaxed);
}

// ============================================
// Socket I/O
// ============================================

bool SlimprotoClient::readExact(void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(m_socket, ptr, remaining, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            return false;
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

bool SlimprotoClient::sendAll(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = send(m_socket, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

bool SlimprotoClient::sendMessage(const char opcode[4], const void* payload, size_t payloadLen) {
    // Client -> Server: [4 opcode][4 length BE][payload]
    std::lock_guard<std::mutex> lock(m_sendMutex);

    uint32_t lenBE = htonl(static_cast<uint32_t>(payloadLen));

    // Send opcode + length + payload in one go to avoid small packets
    std::vector<uint8_t> frame(8 + payloadLen);
    std::memcpy(frame.data(), opcode, 4);
    std::memcpy(frame.data() + 4, &lenBE, 4);
    if (payloadLen > 0 && payload) {
        std::memcpy(frame.data() + 8, payload, payloadLen);
    }

    return sendAll(frame.data(), frame.size());
}

// ============================================
// MAC Address
// ============================================

void SlimprotoClient::generateMac() {
    // Generate deterministic MAC from player name
    // Use a simple hash to get 6 bytes
    std::hash<std::string> hasher;
    size_t h = hasher(m_config.playerName);

    m_mac[0] = 0x02;  // Locally administered, unicast
    m_mac[1] = static_cast<uint8_t>((h >> 0) & 0xFF);
    m_mac[2] = static_cast<uint8_t>((h >> 8) & 0xFF);
    m_mac[3] = static_cast<uint8_t>((h >> 16) & 0xFF);
    m_mac[4] = static_cast<uint8_t>((h >> 24) & 0xFF);
    m_mac[5] = static_cast<uint8_t>((h >> 32) & 0xFF);
}

bool SlimprotoClient::parseMac(const std::string& str) {
    // Parse "xx:xx:xx:xx:xx:xx" or "xx-xx-xx-xx-xx-xx"
    unsigned int bytes[6];
    int n = sscanf(str.c_str(), "%x:%x:%x:%x:%x:%x",
                   &bytes[0], &bytes[1], &bytes[2],
                   &bytes[3], &bytes[4], &bytes[5]);
    if (n != 6) {
        n = sscanf(str.c_str(), "%x-%x-%x-%x-%x-%x",
                   &bytes[0], &bytes[1], &bytes[2],
                   &bytes[3], &bytes[4], &bytes[5]);
    }
    if (n != 6) return false;

    for (int i = 0; i < 6; i++) {
        if (bytes[i] > 0xFF) return false;
        m_mac[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
}

// ============================================
// Capabilities
// ============================================

std::string SlimprotoClient::buildCapabilities() const {
    std::ostringstream caps;

    // Codecs — LMS splits on commas and matches ^[a-z][a-z0-9]{1,4}$
    caps << "flc,pcm,aif,wav";
#ifdef ENABLE_MP3
    caps << ",mp3";
#endif
#ifdef ENABLE_OGG
    caps << ",ogg";
#endif
#ifdef ENABLE_AAC
    caps << ",aac";
#endif
    if (m_config.dsdEnabled) {
        caps << ",dsf,dff";  // DSD container formats recognized by LMS
    }

    // Features — also comma-separated key=value pairs
    // LMS SqueezePlay::updateCapabilities() parses these via split(',')
    caps << ",MaxSampleRate=" << m_config.maxSampleRate;
    caps << ",Model=slim2diretta";
    caps << ",ModelName=slim2diretta";
    caps << ",AccuratePlayPoints=1";
    caps << ",HasDigitalOut=1";

    return caps.str();
}

// ============================================
// Jiffies
// ============================================

uint32_t SlimprotoClient::getJiffies() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime);
    return static_cast<uint32_t>(elapsed.count());
}
