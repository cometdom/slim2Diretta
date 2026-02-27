/**
 * @file DsdStreamReader.h
 * @brief DSF/DFF container parser for DSD audio streams
 *
 * Not a Decoder — DSD is a raw bitstream that doesn't need decoding.
 * This reader parses DSF/DFF container headers and outputs raw planar
 * DSD bytes ready for DirettaSync::sendAudio().
 *
 * Supported containers:
 * - DSF: block-interleaved, LSB-first
 * - DFF (DSDIFF): byte-interleaved, MSB-first
 * - Raw DSD: no container, format from strm parameters
 */

#ifndef SLIM2DIRETTA_DSD_STREAM_READER_H
#define SLIM2DIRETTA_DSD_STREAM_READER_H

#include <cstdint>
#include <cstddef>
#include <vector>

struct DsdFormat {
    uint32_t sampleRate = 0;          // DSD bit rate (e.g., 2822400 for DSD64)
    uint32_t channels = 0;
    uint32_t blockSizePerChannel = 0; // DSF block size (0 for DFF/raw)
    uint64_t totalDsdBytes = 0;       // Total DSD data bytes (all channels, 0=unknown)

    enum class Container { DSF, DFF, RAW };
    Container container = Container::DSF;

    bool isLSBFirst = false;          // DSF=true (LSB first), DFF/RAW=false (MSB first)
};

class DsdStreamReader {
public:
    DsdStreamReader();
    ~DsdStreamReader() = default;

    /**
     * @brief Feed raw container data from HTTP stream
     * @return Number of bytes consumed (always len — buffered internally)
     */
    size_t feed(const uint8_t* data, size_t len);

    /**
     * @brief Signal end of HTTP stream
     */
    void setEof();

    /**
     * @brief Read planar DSD data ready for DirettaSync::sendAudio
     * @param out Output buffer
     * @param maxBytes Maximum bytes to read
     * @return Number of planar bytes written [L0L1...][R0R1...]
     */
    size_t readPlanar(uint8_t* out, size_t maxBytes);

    bool isFormatReady() const { return m_formatReady; }
    const DsdFormat& getFormat() const { return m_format; }
    bool isFinished() const { return m_finished; }
    bool hasError() const { return m_error; }
    uint64_t getTotalBytesOutput() const { return m_totalBytesOutput; }

    /**
     * @brief Set raw DSD format hint from strm parameters (no container)
     */
    void setRawDsdFormat(uint32_t dsdRate, uint32_t channels);

    /**
     * @brief Reset for new stream
     */
    void flush();

private:
    enum class State { DETECT, PARSE_DSF, PARSE_DFF, DATA, DONE, ERROR };

    bool detectContainer();
    bool parseDsfHeader();
    bool parseDffHeader();

    // DSF: block-interleaved → planar (already planar per block pair)
    size_t processDsfBlocks(uint8_t* out, size_t maxBytes);
    // DFF: byte-interleaved → planar via DsdProcessor
    size_t processDffData(uint8_t* out, size_t maxBytes);
    // Raw: byte-interleaved → planar via DsdProcessor
    size_t processRawData(uint8_t* out, size_t maxBytes);

    // Read helpers
    static uint32_t readLE32(const uint8_t* p);
    static uint64_t readLE64(const uint8_t* p);
    static uint32_t readBE32(const uint8_t* p);
    static uint64_t readBE64(const uint8_t* p);

    State m_state = State::DETECT;

    // Header accumulation buffer
    std::vector<uint8_t> m_headerBuf;

    // DSD data buffer (raw bytes from container)
    std::vector<uint8_t> m_dataBuf;

    DsdFormat m_format;
    bool m_formatReady = false;
    bool m_rawDsdConfigured = false;

    uint64_t m_dataRemaining = 0;     // DSD data bytes remaining in container
    uint64_t m_totalBytesOutput = 0;

    bool m_eof = false;
    bool m_error = false;
    bool m_finished = false;
};

#endif // SLIM2DIRETTA_DSD_STREAM_READER_H
