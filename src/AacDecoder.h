/**
 * @file AacDecoder.h
 * @brief AAC stream decoder using fdk-aac
 *
 * Uses fdk-aac with ADTS transport for internet radio streams:
 * - feed() accumulates encoded AAC data into an internal buffer
 * - readDecoded() uses aacDecoder_Fill() + aacDecoder_DecodeFrame()
 *
 * Supports HE-AAC v2 (SBR + PS) natively via fdk-aac.
 * Automatic resync on ADTS sync errors (important for radio streams).
 */

#ifndef SLIM2DIRETTA_AAC_DECODER_H
#define SLIM2DIRETTA_AAC_DECODER_H

#include "Decoder.h"
#include <fdk-aac/aacdecoder_lib.h>
#include <vector>

class AacDecoder : public Decoder {
public:
    AacDecoder();
    ~AacDecoder() override;

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
    HANDLE_AACDECODER m_handle = nullptr;

    // Input buffer (fed by caller)
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;

    // Output buffer (S32_LE interleaved, MSB-aligned)
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    // Temp buffer for fdk-aac output
    std::vector<INT_PCM> m_decodeBuf;

    DecodedFormat m_format;
    bool m_formatReady = false;
    int m_shift = 0;

    bool m_eof = false;
    bool m_error = false;
    bool m_finished = false;
    uint64_t m_decodedSamples = 0;
};

#endif // SLIM2DIRETTA_AAC_DECODER_H
