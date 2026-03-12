/**
 * @file FfmpegDecoder.cpp
 * @brief Audio decoder implementation using FFmpeg (libavcodec/libavformat)
 *
 * Push-based streaming via custom AVIOContext:
 * - avioReadCallback() reads from m_inputBuffer (filled by feed())
 * - avformat_open_input() probes format via AVIO
 * - av_read_frame() + avcodec_send/receive decodes audio
 * - Output normalized to S32_LE interleaved MSB-aligned
 *
 * FFmpeg decodes FLAC/ALAC 24-bit to S32 with data in upper 24 bits
 * (MSB-aligned). For 16-bit sources decoded to S16, we shift left by 16.
 */

#include "FfmpegDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

FfmpegDecoder::FfmpegDecoder() {
    m_outputBuffer.reserve(16384);
}

FfmpegDecoder::~FfmpegDecoder() {
    cleanup();
}

void FfmpegDecoder::cleanup() {
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_fmtCtx) {
        // When using custom AVIO, we must NOT let avformat close the AVIO
        // because we manage m_avioCtx ourselves.
        if (m_fmtCtx->pb == m_avioCtx) {
            m_fmtCtx->pb = nullptr;
        }
        avformat_close_input(&m_fmtCtx);
    }
    if (m_avioCtx) {
        // m_avioBuf is freed by avio_context_free if it was allocated by avio
        avio_context_free(&m_avioCtx);
        m_avioBuf = nullptr;  // was owned by AVIOContext
    } else if (m_avioBuf) {
        av_free(m_avioBuf);
        m_avioBuf = nullptr;
    }
    m_initialized = false;
    m_audioStreamIndex = -1;
}

int FfmpegDecoder::avioReadCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<FfmpegDecoder*>(opaque);

    size_t available = self->m_inputBuffer.size() - self->m_inputPos;

    if (available == 0) {
        if (self->m_eof) {
            return AVERROR_EOF;
        }
        // No data yet — signal "try again"
        return AVERROR(EAGAIN);
    }

    size_t toRead = std::min(available, static_cast<size_t>(buf_size));
    std::memcpy(buf, self->m_inputBuffer.data() + self->m_inputPos, toRead);
    self->m_inputPos += toRead;

    // Compact input buffer periodically
    if (self->m_inputPos > 65536) {
        self->m_inputBuffer.erase(self->m_inputBuffer.begin(),
                                   self->m_inputBuffer.begin() + self->m_inputPos);
        self->m_inputPos = 0;
    }

    return static_cast<int>(toRead);
}

bool FfmpegDecoder::initDecoder() {
    // Allocate AVIO buffer
    m_avioBuf = static_cast<uint8_t*>(av_malloc(AVIO_BUF_SIZE));
    if (!m_avioBuf) {
        LOG_ERROR("[FFmpeg] Failed to allocate AVIO buffer");
        m_error = true;
        return false;
    }

    // Create custom AVIO context (read-only, non-seekable)
    m_avioCtx = avio_alloc_context(
        m_avioBuf,
        AVIO_BUF_SIZE,
        0,              // write_flag = 0 (read only)
        this,           // opaque
        avioReadCallback,
        nullptr,        // write callback
        nullptr         // seek callback (non-seekable stream)
    );
    if (!m_avioCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate AVIO context");
        av_free(m_avioBuf);
        m_avioBuf = nullptr;
        m_error = true;
        return false;
    }

    // Allocate format context with custom I/O
    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate format context");
        m_error = true;
        cleanup();
        return false;
    }
    m_fmtCtx->pb = m_avioCtx;

    // Probe and open input
    // Use a short probe to avoid buffering too much data
    m_fmtCtx->probesize = 32768;
    m_fmtCtx->max_analyze_duration = 500000;  // 500ms

    int ret = avformat_open_input(&m_fmtCtx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[FFmpeg] Failed to open input: " << errbuf);
        m_error = true;
        // avformat_open_input frees m_fmtCtx on failure
        m_fmtCtx = nullptr;
        return false;
    }

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[FFmpeg] Failed to find stream info: " << errbuf);
        m_error = true;
        return false;
    }

    // Find best audio stream
    m_audioStreamIndex = av_find_best_stream(
        m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audioStreamIndex < 0) {
        LOG_ERROR("[FFmpeg] No audio stream found");
        m_error = true;
        return false;
    }

    AVStream* stream = m_fmtCtx->streams[m_audioStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        LOG_ERROR("[FFmpeg] Unsupported codec: "
                  << avcodec_get_name(stream->codecpar->codec_id));
        m_error = true;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate codec context");
        m_error = true;
        return false;
    }

    avcodec_parameters_to_context(m_codecCtx, stream->codecpar);

    // Request S32 output when possible (for FLAC, this gives MSB-aligned 24-bit)
    m_codecCtx->request_sample_fmt = AV_SAMPLE_FMT_S32;

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[FFmpeg] Failed to open codec: " << errbuf);
        m_error = true;
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        LOG_ERROR("[FFmpeg] Failed to allocate frame/packet");
        m_error = true;
        return false;
    }

    // Determine output format and shift
    int bitsPerRawSample = m_codecCtx->bits_per_raw_sample;
    if (bitsPerRawSample == 0) {
        // Fallback: derive from sample format
        switch (m_codecCtx->sample_fmt) {
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                bitsPerRawSample = 16;
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                bitsPerRawSample = 24;  // Assume 24-bit in S32 container
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                bitsPerRawSample = 32;
                break;
            default:
                bitsPerRawSample = 16;
                break;
        }
    }

    m_format.sampleRate = static_cast<uint32_t>(m_codecCtx->sample_rate);
    m_format.channels = static_cast<uint32_t>(m_codecCtx->ch_layout.nb_channels);
    m_format.bitDepth = static_cast<uint32_t>(bitsPerRawSample);
    m_format.totalSamples = 0;

    // Compute shift for MSB alignment in S32
    // FFmpeg FLAC decoder outputs S32 with data already MSB-aligned for 24-bit
    // For S16, we need to shift left by 16
    switch (m_codecCtx->sample_fmt) {
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            m_shift = 16;
            break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            m_shift = 0;  // Already MSB-aligned (FLAC 24-bit) or full 32-bit
            break;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            m_shift = 0;  // Will convert float to S32
            break;
        default:
            m_shift = 0;
            break;
    }

    m_formatReady = true;

    const char* codecName = avcodec_get_name(m_codecCtx->codec_id);
    LOG_INFO("[FFmpeg] Format: " << codecName << " "
             << m_format.sampleRate << " Hz, "
             << m_format.channels << " ch, "
             << bitsPerRawSample << " bit"
             << " (sample_fmt=" << av_get_sample_fmt_name(m_codecCtx->sample_fmt) << ")"
             << " shift=" << m_shift);

    m_initialized = true;
    return true;
}

size_t FfmpegDecoder::feed(const uint8_t* data, size_t len) {
    m_inputBuffer.insert(m_inputBuffer.end(), data, data + len);
    return len;
}

void FfmpegDecoder::setEof() {
    m_eof = true;
}

void FfmpegDecoder::setRawPcmFormat(uint32_t sampleRate, uint32_t bitDepth,
                                      uint32_t channels, bool bigEndian) {
    m_rawPcmConfigured = true;
    m_rawSampleRate = sampleRate;
    m_rawBitDepth = bitDepth;
    m_rawChannels = channels;
    m_rawBigEndian = bigEndian;
}

size_t FfmpegDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init: wait for enough data to probe format
    if (!m_initialized) {
        size_t available = m_inputBuffer.size() - m_inputPos;
        if (available < MIN_PROBE_DATA && !m_eof) {
            return 0;  // Wait for more data
        }
        if (!initDecoder()) {
            return 0;
        }
    }

    size_t channels = m_format.channels;
    if (channels == 0) return 0;

    size_t outputFrames = (m_outputBuffer.size() - m_outputPos) / channels;

    // Decode until we have enough output frames
    while (outputFrames < maxFrames) {
        // Try to receive a decoded frame
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);

        if (ret == 0) {
            // Got a decoded frame — convert to S32_LE interleaved
            int numSamples = m_frame->nb_samples;
            int numChannels = m_frame->ch_layout.nb_channels;

            for (int s = 0; s < numSamples; s++) {
                for (int ch = 0; ch < numChannels; ch++) {
                    int32_t sample = 0;

                    switch (m_codecCtx->sample_fmt) {
                        case AV_SAMPLE_FMT_S16: {
                            const int16_t* data = reinterpret_cast<const int16_t*>(
                                m_frame->data[0]);
                            sample = static_cast<int32_t>(
                                data[s * numChannels + ch]) << 16;
                            break;
                        }
                        case AV_SAMPLE_FMT_S16P: {
                            const int16_t* data = reinterpret_cast<const int16_t*>(
                                m_frame->data[ch]);
                            sample = static_cast<int32_t>(data[s]) << 16;
                            break;
                        }
                        case AV_SAMPLE_FMT_S32: {
                            const int32_t* data = reinterpret_cast<const int32_t*>(
                                m_frame->data[0]);
                            sample = data[s * numChannels + ch];
                            break;
                        }
                        case AV_SAMPLE_FMT_S32P: {
                            const int32_t* data = reinterpret_cast<const int32_t*>(
                                m_frame->data[ch]);
                            sample = data[s];
                            break;
                        }
                        case AV_SAMPLE_FMT_FLT: {
                            const float* data = reinterpret_cast<const float*>(
                                m_frame->data[0]);
                            float f = data[s * numChannels + ch];
                            // Clamp and convert to S32
                            if (f > 1.0f) f = 1.0f;
                            if (f < -1.0f) f = -1.0f;
                            sample = static_cast<int32_t>(f * 2147483647.0f);
                            break;
                        }
                        case AV_SAMPLE_FMT_FLTP: {
                            const float* data = reinterpret_cast<const float*>(
                                m_frame->data[ch]);
                            float f = data[s];
                            if (f > 1.0f) f = 1.0f;
                            if (f < -1.0f) f = -1.0f;
                            sample = static_cast<int32_t>(f * 2147483647.0f);
                            break;
                        }
                        default:
                            break;
                    }

                    m_outputBuffer.push_back(sample);
                }
            }

            av_frame_unref(m_frame);
            outputFrames = (m_outputBuffer.size() - m_outputPos) / channels;
            continue;
        }

        if (ret == AVERROR(EAGAIN)) {
            // Need more input — read a packet
            int readRet = av_read_frame(m_fmtCtx, m_packet);

            if (readRet == 0) {
                if (m_packet->stream_index == m_audioStreamIndex) {
                    avcodec_send_packet(m_codecCtx, m_packet);
                }
                av_packet_unref(m_packet);
                continue;
            }

            if (readRet == AVERROR_EOF) {
                // Flush decoder
                avcodec_send_packet(m_codecCtx, nullptr);
                continue;
            }

            if (readRet == AVERROR(EAGAIN)) {
                // AVIO needs more data
                if (m_eof) {
                    avcodec_send_packet(m_codecCtx, nullptr);
                    continue;
                }
                break;  // Return what we have, caller will feed more data
            }

            // Read error
            char errbuf[128];
            av_strerror(readRet, errbuf, sizeof(errbuf));
            LOG_WARN("[FFmpeg] Read error: " << errbuf);
            if (m_eof) {
                m_finished = true;
            }
            break;
        }

        if (ret == AVERROR_EOF) {
            m_finished = true;
            break;
        }

        // Decode error
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("[FFmpeg] Decode error: " << errbuf);
        break;
    }

    // Copy available output frames
    size_t framesAvailable = (m_outputBuffer.size() - m_outputPos) / channels;
    size_t framesToCopy = std::min(framesAvailable, maxFrames);

    if (framesToCopy > 0) {
        size_t samplesToCopy = framesToCopy * channels;
        std::memcpy(out, m_outputBuffer.data() + m_outputPos,
                    samplesToCopy * sizeof(int32_t));
        m_outputPos += samplesToCopy;
        m_decodedSamples += framesToCopy;

        // Compact output buffer
        if (m_outputPos > 0) {
            m_outputBuffer.erase(m_outputBuffer.begin(),
                                  m_outputBuffer.begin() + m_outputPos);
            m_outputPos = 0;
        }
    }

    return framesToCopy;
}

void FfmpegDecoder::flush() {
    cleanup();
    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_shift = 0;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
    m_probeAttempted = false;
    m_rawPcmConfigured = false;
}
