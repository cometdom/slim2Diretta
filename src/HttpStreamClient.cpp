/**
 * @file HttpStreamClient.cpp
 * @brief HTTP streaming client implementation
 */

#include "HttpStreamClient.h"
#include "LogLevel.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

#include <cstring>
#include <algorithm>

HttpStreamClient::HttpStreamClient() = default;

HttpStreamClient::~HttpStreamClient() {
    disconnect();
}

bool HttpStreamClient::connect(const std::string& serverIp, uint16_t serverPort,
                                const std::string& httpRequest) {
    disconnect();

    m_responseHeaders.clear();
    m_httpStatus = 0;
    m_bytesReceived = 0;
    m_icyMetaInt = 0;
    m_icyBytesUntilMeta = 0;

    // Create TCP socket
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LOG_ERROR("[HTTP] Failed to create socket: " << strerror(errno));
        return false;
    }

    // TCP_NODELAY for responsiveness
    int flag = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Larger receive buffer for streaming
    int rcvBuf = 256 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    // Connect
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("[HTTP] Invalid server address: " << serverIp);
        close(m_socket);
        m_socket = -1;
        return false;
    }

    LOG_DEBUG("[HTTP] Connecting to " << serverIp << ":" << serverPort);

    if (::connect(m_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("[HTTP] Connection failed: " << strerror(errno));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    LOG_DEBUG("[HTTP] Connected, sending request");

    // Send the HTTP request provided by LMS
    if (!sendAll(httpRequest.c_str(), httpRequest.size())) {
        LOG_ERROR("[HTTP] Failed to send request");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Parse response headers
    if (!parseResponseHeaders()) {
        LOG_ERROR("[HTTP] Failed to parse response headers");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_connected.store(true, std::memory_order_release);

    LOG_INFO("[HTTP] Stream connected (status " << m_httpStatus << ")");
    LOG_DEBUG("[HTTP] Response headers:\n" << m_responseHeaders);

    return true;
}

void HttpStreamClient::disconnect() {
    m_connected.store(false, std::memory_order_release);
    if (m_socket >= 0) {
        shutdown(m_socket, SHUT_RDWR);
        close(m_socket);
        m_socket = -1;
    }
}

bool HttpStreamClient::isConnected() const {
    return m_connected.load(std::memory_order_acquire);
}

ssize_t HttpStreamClient::readRaw(uint8_t* buf, size_t maxLen) {
    if (m_socket < 0) return -1;

    ssize_t n = recv(m_socket, buf, maxLen, 0);
    if (n > 0) {
        return n;
    } else if (n == 0) {
        m_connected.store(false, std::memory_order_release);
        return 0;
    } else {
        if (errno == EINTR) return 0;
        LOG_ERROR("[HTTP] Read error: " << strerror(errno));
        m_connected.store(false, std::memory_order_release);
        return -1;
    }
}

bool HttpStreamClient::skipIcyMetadata() {
    // Read 1 byte: metadata length * 16
    uint8_t lenByte = 0;
    while (true) {
        ssize_t n = recv(m_socket, &lenByte, 1, 0);
        if (n == 1) break;
        if (n == 0) {
            m_connected.store(false, std::memory_order_release);
            return false;
        }
        if (errno != EINTR) {
            m_connected.store(false, std::memory_order_release);
            return false;
        }
    }

    size_t metaLen = static_cast<size_t>(lenByte) * 16;
    if (metaLen == 0) return true;  // No metadata this time

    // Read and discard metadata bytes
    uint8_t discardBuf[256];
    size_t remaining = metaLen;
    while (remaining > 0) {
        size_t toRead = std::min(remaining, sizeof(discardBuf));
        ssize_t n = recv(m_socket, discardBuf, toRead, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            m_connected.store(false, std::memory_order_release);
            return false;
        }
        remaining -= static_cast<size_t>(n);
    }

    return true;
}

ssize_t HttpStreamClient::read(uint8_t* buf, size_t maxLen) {
    if (m_socket < 0) return -1;

    // No ICY metadata — simple passthrough
    if (m_icyMetaInt == 0) {
        ssize_t n = readRaw(buf, maxLen);
        if (n > 0) m_bytesReceived += static_cast<uint64_t>(n);
        return n;
    }

    // ICY metadata active — limit read to bytes before next metadata block
    size_t canRead = std::min(maxLen, static_cast<size_t>(m_icyBytesUntilMeta));
    if (canRead == 0) {
        // At metadata boundary — skip metadata block
        if (!skipIcyMetadata()) return -1;
        m_icyBytesUntilMeta = m_icyMetaInt;
        canRead = std::min(maxLen, static_cast<size_t>(m_icyBytesUntilMeta));
    }

    ssize_t n = readRaw(buf, canRead);
    if (n > 0) {
        m_bytesReceived += static_cast<uint64_t>(n);
        m_icyBytesUntilMeta -= static_cast<uint32_t>(n);
    }
    return n;
}

ssize_t HttpStreamClient::readWithTimeout(uint8_t* buf, size_t maxLen, int timeoutMs) {
    if (m_socket < 0) return -1;

    struct pollfd pfd;
    pfd.fd = m_socket;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ready = poll(&pfd, 1, timeoutMs);
    if (ready < 0) {
        if (errno == EINTR) return 0;
        LOG_ERROR("[HTTP] Poll error: " << strerror(errno));
        m_connected.store(false, std::memory_order_release);
        return -1;
    }
    if (ready == 0) return 0;  // Timeout - no data available

    // Check for errors/hangup on the socket
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        m_connected.store(false, std::memory_order_release);
        return -1;
    }

    return read(buf, maxLen);
}

bool HttpStreamClient::sendAll(const void* buf, size_t len) {
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

bool HttpStreamClient::parseResponseHeaders() {
    // Read until we find \r\n\r\n (end of HTTP headers)
    // LMS uses HTTP/1.0 so headers are simple
    std::string headerBuf;
    headerBuf.reserve(4096);

    char c;
    int endSeq = 0;  // Track \r\n\r\n sequence

    while (true) {
        ssize_t n = recv(m_socket, &c, 1, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            LOG_ERROR("[HTTP] Connection closed while reading headers");
            return false;
        }

        headerBuf += c;

        // Detect \r\n\r\n
        if (c == '\r' && (endSeq == 0 || endSeq == 2)) {
            endSeq++;
        } else if (c == '\n' && (endSeq == 1 || endSeq == 3)) {
            endSeq++;
            if (endSeq == 4) break;  // Found end of headers
        } else {
            endSeq = 0;
        }

        // Safety: headers shouldn't be larger than 16KB
        if (headerBuf.size() > 16384) {
            LOG_ERROR("[HTTP] Headers too large (>16KB)");
            return false;
        }
    }

    m_responseHeaders = headerBuf;

    // Parse HTTP status code from first line
    // Expected: "HTTP/1.0 200 OK\r\n" or "ICY 200 OK\r\n"
    size_t spacePos = headerBuf.find(' ');
    if (spacePos != std::string::npos && spacePos + 3 < headerBuf.size()) {
        m_httpStatus = std::atoi(headerBuf.c_str() + spacePos + 1);
    }

    if (m_httpStatus != 200) {
        LOG_WARN("[HTTP] Unexpected status: " << m_httpStatus);
    }

    // Parse icy-metaint header (case-insensitive)
    // Format: "icy-metaint:16000\r\n" or "icy-metaint: 16000\r\n"
    std::string lowerHeaders = headerBuf;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    size_t icyPos = lowerHeaders.find("icy-metaint:");
    if (icyPos != std::string::npos) {
        size_t valStart = icyPos + 12;  // strlen("icy-metaint:")
        // Skip optional whitespace
        while (valStart < lowerHeaders.size() && lowerHeaders[valStart] == ' ') {
            valStart++;
        }
        uint32_t metaInt = static_cast<uint32_t>(std::atoi(headerBuf.c_str() + valStart));
        if (metaInt > 0) {
            m_icyMetaInt = metaInt;
            m_icyBytesUntilMeta = metaInt;
            LOG_INFO("[HTTP] ICY metadata interval: " << metaInt << " bytes");
        }
    }

    return true;
}
