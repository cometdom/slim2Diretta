/**
 * @file FfmpegDecoder.h
 * @brief Audio decoder using FFmpeg (libavcodec/libavformat)
 *
 * Alternative to the native decoders (FlacDecoder, PcmDecoder, etc.)
 * for users who prefer FFmpeg's decoding pipeline.
 *
 * Uses a custom AVIOContext for push-based streaming:
 * - feed() pushes encoded data into an internal buffer
 * - readDecoded() pulls decoded S32_LE interleaved samples
 *
 * Supports all formats FFmpeg can decode (FLAC, MP3, AAC, OGG, WAV, AIFF, ALAC...).
 * Output is always S32_LE interleaved, MSB-aligned — same as native decoders.
 */

#ifndef SLIM2DIRETTA_FFMPEG_DECODER_H
#define SLIM2DIRETTA_FFMPEG_DECODER_H

#include "Decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <vector>
#include <cstdint>

class FfmpegDecoder : public Decoder {
public:
    FfmpegDecoder();
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

    // Custom AVIO read callback (reads from m_inputBuffer)
    static int avioReadCallback(void* opaque, uint8_t* buf, int buf_size);

    // Input buffer (fed by caller)
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;
    bool m_eof = false;

    // Output buffer (decoded S32_LE interleaved)
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    // FFmpeg contexts
    AVIOContext* m_avioCtx = nullptr;
    uint8_t* m_avioBuf = nullptr;           // AVIO internal buffer (owned by AVIOContext)
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    int m_audioStreamIndex = -1;

    // Format info
    DecodedFormat m_format;
    bool m_formatReady = false;
    int m_shift = 0;                        // Left shift for MSB alignment

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
    bool m_probeAttempted = false;

    static constexpr size_t AVIO_BUF_SIZE = 32768;
    static constexpr size_t MIN_PROBE_DATA = 8192;
};

#endif // SLIM2DIRETTA_FFMPEG_DECODER_H
