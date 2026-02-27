/**
 * @file PcmDecoder.h
 * @brief PCM decoder for WAV (RIFF) and AIFF container formats
 *
 * Parses container headers, then passes through raw PCM data
 * normalized to S32_LE interleaved MSB-aligned format.
 */

#ifndef SLIM2DIRETTA_PCM_DECODER_H
#define SLIM2DIRETTA_PCM_DECODER_H

#include "Decoder.h"
#include <vector>

class PcmDecoder : public Decoder {
public:
    PcmDecoder();
    ~PcmDecoder() override = default;

    size_t feed(const uint8_t* data, size_t len) override;
    void setEof() override;
    size_t readDecoded(int32_t* out, size_t maxFrames) override;
    bool isFormatReady() const override { return m_formatReady; }
    DecodedFormat getFormat() const override { return m_format; }
    bool isFinished() const override { return m_finished; }
    bool hasError() const override { return m_error; }
    uint64_t getDecodedSamples() const override { return m_decodedSamples; }
    void flush() override;
    void setRawPcmFormat(uint32_t sampleRate, uint32_t bitDepth,
                          uint32_t channels, bool bigEndian) override;

private:
    enum class State { DETECT, PARSE_WAV, PARSE_AIFF, DATA, DONE, ERROR };

    bool detectContainer();
    bool parseWavHeader();
    bool parseAiffHeader();
    size_t convertSamples(const uint8_t* src, int32_t* dst, size_t srcBytes);

    // 80-bit extended float to uint32 (for AIFF sample rate)
    static uint32_t extendedToUint32(const uint8_t* bytes);

    State m_state = State::DETECT;

    // Header accumulation buffer
    std::vector<uint8_t> m_headerBuf;

    // PCM data buffer (raw bytes before conversion)
    std::vector<uint8_t> m_dataBuf;

    // Format
    DecodedFormat m_format;
    bool m_formatReady = false;
    bool m_bigEndian = false;   // AIFF = big-endian PCM
    int m_shift = 0;            // Left shift for MSB alignment

    // Data tracking
    uint64_t m_dataRemaining = 0;  // Bytes remaining in data chunk (0 = unlimited)
    bool m_rawPcmConfigured = false;  // Raw PCM format from strm command
    bool m_eof = false;
    bool m_error = false;
    bool m_finished = false;
    uint64_t m_decodedSamples = 0;
};

#endif // SLIM2DIRETTA_PCM_DECODER_H
