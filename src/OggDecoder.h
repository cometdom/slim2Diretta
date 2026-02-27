/**
 * @file OggDecoder.h
 * @brief Ogg Vorbis stream decoder using libvorbisfile
 *
 * Uses libvorbisfile with custom non-seekable callbacks for streaming:
 * - feed() accumulates encoded Ogg data into an internal buffer
 * - readDecoded() pulls decoded S32_LE interleaved samples via ov_read()
 *
 * Handles chained Ogg streams (format changes) and OV_HOLE gaps
 * (normal for internet radio streams).
 */

#ifndef SLIM2DIRETTA_OGG_DECODER_H
#define SLIM2DIRETTA_OGG_DECODER_H

#include "Decoder.h"
#include <vorbis/vorbisfile.h>
#include <vector>

class OggDecoder : public Decoder {
public:
    OggDecoder();
    ~OggDecoder() override;

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
    // Custom read callback for vorbisfile (non-seekable stream)
    static size_t readCallback(void* ptr, size_t size, size_t nmemb, void* datasource);

    OggVorbis_File m_vf;
    bool m_vfOpen = false;

    // Input buffer (fed by caller, consumed by readCallback)
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;

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
    int m_currentBitstream = -1;
};

#endif // SLIM2DIRETTA_OGG_DECODER_H
