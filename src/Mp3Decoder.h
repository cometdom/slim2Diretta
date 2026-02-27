/**
 * @file Mp3Decoder.h
 * @brief MP3 stream decoder using libmpg123
 *
 * Uses mpg123 in feed mode for push/pull streaming:
 * - feed() pushes encoded MP3 data via mpg123_feed()
 * - readDecoded() pulls decoded S32_LE interleaved samples via mpg123_read()
 *
 * Handles ID3v2 tags, VBR streams, and automatic resync on errors
 * (important for internet radio streams).
 */

#ifndef SLIM2DIRETTA_MP3_DECODER_H
#define SLIM2DIRETTA_MP3_DECODER_H

#include "Decoder.h"
#include <mpg123.h>
#include <vector>
#include <mutex>

class Mp3Decoder : public Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder() override;

    size_t feed(const uint8_t* data, size_t len) override;
    void setEof() override;
    size_t readDecoded(int32_t* out, size_t maxFrames) override;
    bool isFormatReady() const override { return m_formatReady; }
    DecodedFormat getFormat() const override { return m_format; }
    bool isFinished() const override { return m_finished; }
    bool hasError() const override { return m_error; }
    uint64_t getDecodedSamples() const override { return m_decodedSamples; }
    void flush() override;

private:
    bool initHandle();

    mpg123_handle* m_handle = nullptr;

    // Output buffer (S32_LE interleaved, MSB-aligned)
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    DecodedFormat m_format;
    bool m_formatReady = false;

    bool m_eof = false;
    bool m_error = false;
    bool m_finished = false;
    bool m_initialized = false;
    uint64_t m_decodedSamples = 0;

    static std::once_flag s_initFlag;
};

#endif // SLIM2DIRETTA_MP3_DECODER_H
