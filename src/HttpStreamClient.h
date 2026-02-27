/**
 * @file HttpStreamClient.h
 * @brief HTTP streaming client for fetching audio from LMS
 *
 * Connects to LMS HTTP port and streams encoded audio data.
 * The HTTP request is provided by LMS in the strm-s command.
 */

#ifndef SLIM2DIRETTA_HTTP_STREAM_CLIENT_H
#define SLIM2DIRETTA_HTTP_STREAM_CLIENT_H

#include <string>
#include <atomic>
#include <cstdint>

class HttpStreamClient {
public:
    HttpStreamClient();
    ~HttpStreamClient();

    // Non-copyable
    HttpStreamClient(const HttpStreamClient&) = delete;
    HttpStreamClient& operator=(const HttpStreamClient&) = delete;

    // Connect and send the HTTP request provided by LMS
    // serverIp: from strm command (or control connection IP if 0)
    // serverPort: from strm command (typically 9000)
    // httpRequest: full GET request string from strm command
    bool connect(const std::string& serverIp, uint16_t serverPort,
                 const std::string& httpRequest);

    void disconnect();
    bool isConnected() const;

    // Read audio data (blocking). Returns bytes read, 0 = EOF, -1 = error
    ssize_t read(uint8_t* buf, size_t maxLen);

    // Read with timeout using poll(). Returns bytes read, 0 = timeout/no data, -1 = error
    // Negative bytesRead with isConnected()=false means real error/EOF.
    ssize_t readWithTimeout(uint8_t* buf, size_t maxLen, int timeoutMs);

    // HTTP response headers (available after connect)
    const std::string& getResponseHeaders() const { return m_responseHeaders; }
    int getHttpStatus() const { return m_httpStatus; }

    // Total bytes received (audio data only, after headers)
    uint64_t getBytesReceived() const { return m_bytesReceived; }

    // ICY metadata interval (0 = no ICY metadata in stream)
    uint32_t getIcyMetaInt() const { return m_icyMetaInt; }

private:
    int m_socket = -1;
    std::atomic<bool> m_connected{false};

    std::string m_responseHeaders;
    int m_httpStatus = 0;
    uint64_t m_bytesReceived = 0;

    // ICY metadata handling
    uint32_t m_icyMetaInt = 0;        // Metadata interval (bytes), 0 = disabled
    uint32_t m_icyBytesUntilMeta = 0; // Countdown to next metadata block

    // Low-level recv (no ICY handling)
    ssize_t readRaw(uint8_t* buf, size_t maxLen);
    // Read and discard ICY metadata block at current position
    bool skipIcyMetadata();

    bool sendAll(const void* buf, size_t len);
    bool parseResponseHeaders();
};

#endif // SLIM2DIRETTA_HTTP_STREAM_CLIENT_H
