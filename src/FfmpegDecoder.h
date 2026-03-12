/**
 * @file FfmpegDecoder.h
 * @brief Audio decoder using FFmpeg's libavcodec (parser-based, no avformat)
 *
 * Alternative to the native decoders (FlacDecoder, PcmDecoder, etc.)
 * for users who prefer FFmpeg's decoding pipeline.
 *
 * Uses AVCodecParser + AVCodecContext directly — no avformat/AVIO needed.
 * The codec is known from the Slimproto format code, so no probing required.
 *
 * - feed() pushes encoded data into an internal buffer
 * - readDecoded() parses frames and pulls decoded S32_LE interleaved samples
 *
 * Output is always S32_LE interleaved, MSB-aligned — same as native decoders.
 */

#ifndef SLIM2DIRETTA_FFMPEG_DECODER_H
#define SLIM2DIRETTA_FFMPEG_DECODER_H

#include "Decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <vector>
#include <cstdint>

class FfmpegDecoder : public Decoder {
public:
    explicit FfmpegDecoder(char formatCode = 'f');
    ~FfmpegDecoder() override;

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
    bool initDecoder();
    void cleanup();
    void convertFrame();

    // Map Slimproto format code to FFmpeg codec ID
    static AVCodecID formatCodeToCodecId(char code);

    char m_formatCode;

    // Input buffer (fed by caller)
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;
    bool m_eof = false;

    // Output buffer (decoded S32_LE interleaved)
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    // FFmpeg contexts (parser-based, no avformat)
    AVCodecParserContext* m_parser = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;

    // Format info
    DecodedFormat m_format;
    bool m_formatReady = false;

    // Raw PCM hint (from strm command, for headerless PCM)
    bool m_rawPcmConfigured = false;
    uint32_t m_rawSampleRate = 0;
    uint32_t m_rawBitDepth = 0;
    uint32_t m_rawChannels = 0;
    bool m_rawBigEndian = false;

    // State
    bool m_initialized = false;
    bool m_error = false;
    bool m_finished = false;
    uint64_t m_decodedSamples = 0;
};

#endif // SLIM2DIRETTA_FFMPEG_DECODER_H
