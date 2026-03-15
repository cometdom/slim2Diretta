/**
 * @file FfmpegDecoder.h
 * @brief Audio decoder using FFmpeg (avformat/AVIO for containers, direct codec for raw PCM)
 *
 * Two internal modes:
 *
 * Mode A — avformat/AVIO (FLAC, MP3, AAC, Ogg, ALAC):
 *   Custom AVIO read callback bridges the push-based Decoder interface
 *   with FFmpeg's pull-based avformat demuxer. feed() fills an internal
 *   buffer; av_read_frame() triggers the AVIO callback which drains it.
 *   This is the same proven path used by DirettaRendererUPnP's AudioEngine.
 *
 * Mode B — direct codec (raw PCM, format code 'p'):
 *   No container headers → no demuxer. Data is sent directly to avcodec
 *   with manual block_align alignment. Parser is disabled to avoid
 *   FFmpeg splitting packets without respecting block_align.
 *
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
    // Initialization
    bool initAvformat();    // Mode A: avformat/AVIO for containerized formats
    bool initRawPcm();      // Mode B: direct codec for raw PCM
    void cleanup();
    void convertFrame();
    void detectFormat();

    // AVIO read callback (bridges push-based feed() with pull-based avformat)
    static int avioReadCallback(void* opaque, uint8_t* buf, int buf_size);

    // Map Slimproto format code to FFmpeg codec ID (raw PCM only)
    static AVCodecID formatCodeToCodecId(char code);

    char m_formatCode;
    bool m_useAvformat = false;  // true = Mode A, false = Mode B

    // ── Input buffer (shared by both modes) ──
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;
    bool m_eof = false;

    // Minimum bytes before attempting avformat_open_input
    // (needs enough for header probing: FLAC streaminfo, MP3 sync, etc.)
    static constexpr size_t MIN_PROBE_BYTES = 131072;  // 128KB

    // ── Mode A: avformat/AVIO contexts ──
    AVFormatContext* m_fmtCtx = nullptr;
    AVIOContext* m_avioCtx = nullptr;
    uint8_t* m_avioBuf = nullptr;
    int m_audioStreamIdx = -1;
    static constexpr size_t AVIO_BUF_SIZE = 32768;  // 32KB AVIO internal buffer

    // ── Mode B: raw PCM state ──
    bool m_parserFlushed = false;

    // ── Shared codec contexts ──
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;

    // ── Output buffer (decoded S32_LE interleaved) ──
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    // ── Format info ──
    DecodedFormat m_format;
    bool m_formatReady = false;

    // ── Raw PCM hint (from strm command, for headerless PCM) ──
    bool m_rawPcmConfigured = false;
    uint32_t m_rawSampleRate = 0;
    uint32_t m_rawBitDepth = 0;
    uint32_t m_rawChannels = 0;
    bool m_rawBigEndian = false;

    // ── State ──
    bool m_initialized = false;
    bool m_error = false;
    bool m_finished = false;
    uint64_t m_decodedSamples = 0;
};

#endif // SLIM2DIRETTA_FFMPEG_DECODER_H
