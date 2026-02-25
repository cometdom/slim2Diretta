/**
 * @file Decoder.h
 * @brief Abstract decoder interface for audio stream decoding
 *
 * All decoders normalize output to S32_LE interleaved, MSB-aligned.
 * This matches squeezelite's internal format and what DirettaSync expects.
 */

#ifndef SLIM2DIRETTA_DECODER_H
#define SLIM2DIRETTA_DECODER_H

#include <cstdint>
#include <cstddef>
#include <memory>

struct DecodedFormat {
    uint32_t sampleRate = 0;
    uint32_t bitDepth = 0;      // Original bit depth (16, 24, 32)
    uint32_t channels = 0;
    uint64_t totalSamples = 0;  // Per channel, 0 if unknown
};

class Decoder {
public:
    virtual ~Decoder() = default;

    // Non-copyable
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /**
     * @brief Feed encoded data into the decoder
     * @return Number of bytes consumed
     */
    virtual size_t feed(const uint8_t* data, size_t len) = 0;

    /**
     * @brief Signal that no more input data will arrive
     */
    virtual void setEof() = 0;

    /**
     * @brief Read decoded audio frames (S32_LE interleaved, MSB-aligned)
     * @param out Output buffer (must hold maxFrames * channels int32_t values)
     * @param maxFrames Maximum number of frames to read
     * @return Number of frames actually written (0 = need more input)
     */
    virtual size_t readDecoded(int32_t* out, size_t maxFrames) = 0;

    /**
     * @brief Check if audio format has been determined
     */
    virtual bool isFormatReady() const = 0;

    /**
     * @brief Get the decoded audio format (valid after isFormatReady())
     */
    virtual DecodedFormat getFormat() const = 0;

    /**
     * @brief Check if decoder has processed all input and output
     */
    virtual bool isFinished() const = 0;

    /**
     * @brief Check if a fatal error occurred
     */
    virtual bool hasError() const = 0;

    /**
     * @brief Get total number of decoded samples (per channel)
     */
    virtual uint64_t getDecodedSamples() const = 0;

    /**
     * @brief Reset decoder state for new stream
     */
    virtual void flush() = 0;

    /**
     * @brief Create decoder for the given Slimproto format code
     * @param formatCode 'f' = FLAC, 'p' = PCM (WAV/AIFF), 'a' = AAC, etc.
     * @return Decoder instance, or nullptr for unsupported formats
     */
    static std::unique_ptr<Decoder> create(char formatCode);
};

#endif // SLIM2DIRETTA_DECODER_H
