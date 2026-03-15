/**
 * @file FfmpegDecoder.cpp
 * @brief Audio decoder using FFmpeg (avformat/AVIO + direct codec)
 *
 * Two modes:
 *
 * Mode A — avformat/AVIO (FLAC 'f', MP3 'm', AAC 'a', Ogg 'o', ALAC 'l'):
 *   A custom AVIO context bridges our push-based feed() with FFmpeg's
 *   pull-based av_read_frame(). avformat handles demuxing and framing,
 *   eliminating all parser-related issues (block_align, partial frames,
 *   flush). This is the same proven approach as DirettaRendererUPnP.
 *
 * Mode B — direct codec (raw PCM 'p'):
 *   No container → no demuxer. Data sent directly to avcodec with manual
 *   block_align alignment. Parser disabled (some FFmpeg versions split
 *   PCM data without respecting block_align).
 */

#include "FfmpegDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

// ============================================================================
// Helpers
// ============================================================================

AVCodecID FfmpegDecoder::formatCodeToCodecId(char code) {
    switch (code) {
        case 'f': return AV_CODEC_ID_FLAC;
        case 'm': return AV_CODEC_ID_MP3;
        case 'a': return AV_CODEC_ID_AAC;
        case 'o': return AV_CODEC_ID_VORBIS;
        case 'l': return AV_CODEC_ID_ALAC;
        case 'p': return AV_CODEC_ID_PCM_S16LE;  // Refined by setRawPcmFormat
        default:  return AV_CODEC_ID_NONE;
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

FfmpegDecoder::FfmpegDecoder(char formatCode)
    : m_formatCode(formatCode)
    , m_useAvformat(formatCode != 'p')
{
    m_outputBuffer.reserve(16384);
}

FfmpegDecoder::~FfmpegDecoder() {
    cleanup();
}

// ============================================================================
// Decoder interface
// ============================================================================

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

void FfmpegDecoder::flush() {
    cleanup();
    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_parserFlushed = false;
    m_decodedSamples = 0;
    m_rawPcmConfigured = false;
}

// ============================================================================
// AVIO read callback (Mode A)
// ============================================================================

int FfmpegDecoder::avioReadCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* self = static_cast<FfmpegDecoder*>(opaque);

    size_t available = self->m_inputBuffer.size() - self->m_inputPos;
    if (available == 0) {
        if (self->m_eof) return AVERROR_EOF;
        // No data yet — return AVERROR_EOF to unblock avformat.
        // The caller (readDecoded) will detect the incomplete init
        // and retry when more data arrives.
        return AVERROR_EOF;
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

// ============================================================================
// Mode A: avformat/AVIO initialization
// ============================================================================

bool FfmpegDecoder::initAvformat() {
    // Allocate AVIO buffer
    m_avioBuf = static_cast<uint8_t*>(av_malloc(AVIO_BUF_SIZE));
    if (!m_avioBuf) {
        LOG_ERROR("[FFmpeg] Failed to allocate AVIO buffer");
        m_error = true;
        return false;
    }

    // Create custom AVIO context
    m_avioCtx = avio_alloc_context(
        m_avioBuf, static_cast<int>(AVIO_BUF_SIZE),
        0,          // write_flag = 0 (read-only)
        this,       // opaque pointer
        avioReadCallback,
        nullptr,    // write callback
        nullptr     // seek callback
    );
    if (!m_avioCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate AVIO context");
        av_free(m_avioBuf);
        m_avioBuf = nullptr;
        m_error = true;
        return false;
    }

    // Create format context with our custom AVIO
    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate format context");
        m_error = true;
        return false;
    }
    m_fmtCtx->pb = m_avioCtx;
    // Minimize probing — we provide the format hint and don't call
    // avformat_find_stream_info, so only the container header is needed.
    m_fmtCtx->probesize = 32768;
    m_fmtCtx->max_analyze_duration = 0;

    // Hint the input format when we know it from Slimproto
    const AVInputFormat* inputFmt = nullptr;
    switch (m_formatCode) {
        case 'f': inputFmt = av_find_input_format("flac"); break;
        case 'm': inputFmt = av_find_input_format("mp3"); break;
        case 'a': inputFmt = av_find_input_format("aac"); break;
        case 'o': inputFmt = av_find_input_format("ogg"); break;
        case 'l': inputFmt = av_find_input_format("mov"); break; // ALAC in MOV
        default: break;
    }

    // Open input via AVIO
    int ret = avformat_open_input(&m_fmtCtx, nullptr, inputFmt, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("[FFmpeg] avformat_open_input failed: " << errbuf);
        // m_fmtCtx is freed by avformat_open_input on failure
        m_fmtCtx = nullptr;
        m_error = true;
        return false;
    }

    // Minimal stream analysis — probesize and max_analyze_duration are
    // already set to small values above, so this reads very little data.
    // We provide the format hint, so only the container header is needed.
    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("[FFmpeg] avformat_find_stream_info: " << errbuf
                 << " (continuing anyway)");
    }
    // Reset AVIO EOF flag in case probing hit buffer exhaustion
    if (m_avioCtx) {
        m_avioCtx->eof_reached = 0;
    }

    // Find audio stream
    m_audioStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO,
                                            -1, -1, nullptr, 0);
    if (m_audioStreamIdx < 0) {
        LOG_ERROR("[FFmpeg] No audio stream found");
        m_error = true;
        return false;
    }

    AVCodecParameters* codecpar = m_fmtCtx->streams[m_audioStreamIdx]->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LOG_ERROR("[FFmpeg] Codec not found for stream");
        m_error = true;
        return false;
    }

    // Allocate and configure codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate codec context");
        m_error = true;
        return false;
    }

    avcodec_parameters_to_context(m_codecCtx, codecpar);

    // Request S32 output for best precision (MSB-aligned 24-bit in S32)
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

    // Detect format from stream info
    detectFormat();

    LOG_INFO("[FFmpeg] avformat ready: " << codec->name
             << " (format code '" << m_formatCode << "'"
             << ", stream #" << m_audioStreamIdx << ")");

    m_initialized = true;
    return true;
}

// ============================================================================
// Mode B: raw PCM initialization (no demuxer)
// ============================================================================

bool FfmpegDecoder::initRawPcm() {
    AVCodecID codecId = formatCodeToCodecId(m_formatCode);

    // Refine PCM codec based on raw format hint
    if (m_rawPcmConfigured) {
        if (m_rawBigEndian) {
            switch (m_rawBitDepth) {
                case 16: codecId = AV_CODEC_ID_PCM_S16BE; break;
                case 24: codecId = AV_CODEC_ID_PCM_S24BE; break;
                case 32: codecId = AV_CODEC_ID_PCM_S32BE; break;
                default: codecId = AV_CODEC_ID_PCM_S16BE; break;
            }
        } else {
            switch (m_rawBitDepth) {
                case 16: codecId = AV_CODEC_ID_PCM_S16LE; break;
                case 24: codecId = AV_CODEC_ID_PCM_S24LE; break;
                case 32: codecId = AV_CODEC_ID_PCM_S32LE; break;
                default: codecId = AV_CODEC_ID_PCM_S16LE; break;
            }
        }
    }

    if (codecId == AV_CODEC_ID_NONE) {
        LOG_ERROR("[FFmpeg] No codec for format code '" << m_formatCode << "'");
        m_error = true;
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        LOG_ERROR("[FFmpeg] Codec not found: " << avcodec_get_name(codecId));
        m_error = true;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate codec context");
        m_error = true;
        return false;
    }

    // Set codec parameters for raw PCM
    if (m_rawPcmConfigured) {
        m_codecCtx->sample_rate = static_cast<int>(m_rawSampleRate);
        AVChannelLayout layout = {};
        av_channel_layout_default(&layout, static_cast<int>(m_rawChannels));
        av_channel_layout_copy(&m_codecCtx->ch_layout, &layout);
        av_channel_layout_uninit(&layout);

        int bytesPerSample = static_cast<int>(m_rawBitDepth) / 8;
        m_codecCtx->block_align = static_cast<int>(m_rawChannels) * bytesPerSample;
        LOG_DEBUG("[FFmpeg] Raw PCM block_align set to "
                  << m_codecCtx->block_align << " (" << m_rawChannels
                  << " ch × " << bytesPerSample << " bytes)");
    }

    m_codecCtx->request_sample_fmt = AV_SAMPLE_FMT_S32;

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
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

    LOG_INFO("[FFmpeg] Decoder ready: " << codec->name
             << " (format code '" << m_formatCode << "', raw PCM mode)");

    m_initialized = true;
    return true;
}

// ============================================================================
// Format detection
// ============================================================================

void FfmpegDecoder::detectFormat() {
    if (m_formatReady) return;

    if (m_useAvformat && m_fmtCtx && m_audioStreamIdx >= 0) {
        // Mode A: detect from stream info (available immediately)
        AVCodecParameters* codecpar = m_fmtCtx->streams[m_audioStreamIdx]->codecpar;

        m_format.sampleRate = static_cast<uint32_t>(codecpar->sample_rate);
        m_format.channels = static_cast<uint32_t>(codecpar->ch_layout.nb_channels);

        // Detect real bit depth (same logic as DirettaRendererUPnP)
        int realBitDepth = 0;

        // Method 1: bits_per_raw_sample (most reliable for FLAC/ALAC)
        if (codecpar->bits_per_raw_sample > 0 && codecpar->bits_per_raw_sample <= 32) {
            realBitDepth = codecpar->bits_per_raw_sample;
        }
        // Method 2: codec ID (for PCM formats)
        else if (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
                 codecpar->codec_id == AV_CODEC_ID_PCM_S16BE) {
            realBitDepth = 16;
        }
        else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
                 codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
            realBitDepth = 24;
        }
        else if (codecpar->codec_id == AV_CODEC_ID_PCM_S32LE ||
                 codecpar->codec_id == AV_CODEC_ID_PCM_S32BE) {
            realBitDepth = 32;
        }
        // Method 3: fallback to sample format
        else {
            switch (codecpar->format) {
                case AV_SAMPLE_FMT_S16:
                case AV_SAMPLE_FMT_S16P:
                    realBitDepth = 16; break;
                case AV_SAMPLE_FMT_S32:
                case AV_SAMPLE_FMT_S32P:
                    realBitDepth = 32; break;
                case AV_SAMPLE_FMT_FLT:
                case AV_SAMPLE_FMT_FLTP:
                    realBitDepth = 32; break;
                default:
                    realBitDepth = 24; break;
            }
        }

        // FLAC 24-bit is decoded into S32 with samples in upper 24 bits
        if (realBitDepth == 32 &&
            (codecpar->codec_id == AV_CODEC_ID_FLAC ||
             codecpar->codec_id == AV_CODEC_ID_ALAC) &&
            codecpar->bits_per_raw_sample == 24) {
            realBitDepth = 24;
        }

        m_format.bitDepth = static_cast<uint32_t>(realBitDepth);
        m_format.totalSamples = 0;
        m_formatReady = true;

        LOG_INFO("[FFmpeg] Format: "
                 << avcodec_get_name(codecpar->codec_id) << " "
                 << m_format.sampleRate << " Hz, "
                 << m_format.channels << " ch, "
                 << m_format.bitDepth << " bit");
    }
}

// ============================================================================
// Frame conversion (S32_LE interleaved, MSB-aligned)
// ============================================================================

void FfmpegDecoder::convertFrame() {
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
}

// ============================================================================
// readDecoded — Mode A: avformat/AVIO
// ============================================================================

size_t FfmpegDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init
    if (!m_initialized) {
        if (m_useAvformat) {
            // Wait for enough data to probe headers
            size_t available = m_inputBuffer.size() - m_inputPos;
            if (available < MIN_PROBE_BYTES && !m_eof) {
                return 0;  // Need more data
            }
            if (!initAvformat()) return 0;
        } else {
            if (!initRawPcm()) return 0;
        }
    }

    if (m_useAvformat) {
        // ── Mode A: avformat/AVIO ──
        // Pull packets via av_read_frame → decode → convert
        while (true) {
            // 1. Try to receive decoded frames
            int ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == 0) {
                // Detect format from first decoded frame (Mode B fallback)
                if (!m_formatReady) {
                    detectFormat();
                    // If still not ready, detect from codec context
                    if (!m_formatReady) {
                        m_format.sampleRate = static_cast<uint32_t>(m_codecCtx->sample_rate);
                        m_format.channels = static_cast<uint32_t>(m_codecCtx->ch_layout.nb_channels);
                        int bps = m_codecCtx->bits_per_raw_sample;
                        if (bps == 0) {
                            switch (m_codecCtx->sample_fmt) {
                                case AV_SAMPLE_FMT_S16:
                                case AV_SAMPLE_FMT_S16P: bps = 16; break;
                                case AV_SAMPLE_FMT_S32:
                                case AV_SAMPLE_FMT_S32P: bps = 24; break;
                                default: bps = 16; break;
                            }
                        }
                        m_format.bitDepth = static_cast<uint32_t>(bps);
                        m_format.totalSamples = 0;
                        m_formatReady = true;

                        LOG_INFO("[FFmpeg] Format (from frame): "
                                 << m_format.sampleRate << " Hz, "
                                 << m_format.channels << " ch, "
                                 << m_format.bitDepth << " bit");
                    }
                }

                convertFrame();
                av_frame_unref(m_frame);

                size_t channels = m_format.channels;
                if (channels > 0) {
                    size_t framesAvail = (m_outputBuffer.size() - m_outputPos) / channels;
                    if (framesAvail >= maxFrames) break;
                }
                continue;
            }

            if (ret == AVERROR_EOF) {
                m_finished = true;
                break;
            }

            if (ret != AVERROR(EAGAIN)) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARN("[FFmpeg] Decode error: " << errbuf);
                break;
            }

            // 2. EAGAIN: need more packets from avformat
            ret = av_read_frame(m_fmtCtx, m_packet);
            if (ret == 0) {
                if (m_packet->stream_index == m_audioStreamIdx) {
                    int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
                    if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                        char errbuf[128];
                        av_strerror(sendRet, errbuf, sizeof(errbuf));
                        LOG_WARN("[FFmpeg] Send packet error: " << errbuf);
                    }
                }
                av_packet_unref(m_packet);
                continue;
            }

            if (ret == AVERROR_EOF || ret == AVERROR(EIO)) {
                if (m_eof) {
                    // Real EOF — flush decoder to drain buffered frames
                    avcodec_send_packet(m_codecCtx, nullptr);
                    continue;
                }
                // Temporary data starvation — AVIO returned EOF because
                // the input buffer was empty, not because the stream ended.
                // Reset AVIO's EOF flag so av_read_frame will try again
                // on the next call when more data has been fed.
                if (m_avioCtx) {
                    m_avioCtx->eof_reached = 0;
                }
                break;  // Need more feed() data
            }
        }
    } else {
        // ── Mode B: raw PCM direct codec ──
        while (true) {
            int ret = avcodec_receive_frame(m_codecCtx, m_frame);
            if (ret == 0) {
                if (!m_formatReady) {
                    int bitsPerRawSample = m_codecCtx->bits_per_raw_sample;
                    if (bitsPerRawSample == 0) {
                        switch (m_codecCtx->sample_fmt) {
                            case AV_SAMPLE_FMT_S16:
                            case AV_SAMPLE_FMT_S16P: bitsPerRawSample = 16; break;
                            case AV_SAMPLE_FMT_S32:
                            case AV_SAMPLE_FMT_S32P: bitsPerRawSample = 24; break;
                            case AV_SAMPLE_FMT_FLT:
                            case AV_SAMPLE_FMT_FLTP: bitsPerRawSample = 32; break;
                            default: bitsPerRawSample = 16; break;
                        }
                    }
                    m_format.sampleRate = static_cast<uint32_t>(m_codecCtx->sample_rate);
                    m_format.channels = static_cast<uint32_t>(m_codecCtx->ch_layout.nb_channels);
                    m_format.bitDepth = static_cast<uint32_t>(bitsPerRawSample);
                    m_format.totalSamples = 0;
                    m_formatReady = true;

                    LOG_INFO("[FFmpeg] Format: "
                             << avcodec_get_name(m_codecCtx->codec_id) << " "
                             << m_format.sampleRate << " Hz, "
                             << m_format.channels << " ch, "
                             << bitsPerRawSample << " bit"
                             << " (sample_fmt="
                             << av_get_sample_fmt_name(m_codecCtx->sample_fmt) << ")");
                }

                convertFrame();
                av_frame_unref(m_frame);

                size_t channels = m_format.channels;
                if (channels > 0) {
                    size_t framesAvail = (m_outputBuffer.size() - m_outputPos) / channels;
                    if (framesAvail >= maxFrames) break;
                }
                continue;
            }

            if (ret == AVERROR_EOF) {
                m_finished = true;
                break;
            }

            if (ret != AVERROR(EAGAIN)) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARN("[FFmpeg] Decode error: " << errbuf);
                break;
            }

            // EAGAIN: send more raw PCM data
            size_t available = m_inputBuffer.size() - m_inputPos;
            if (available == 0) {
                if (m_eof) {
                    avcodec_send_packet(m_codecCtx, nullptr);
                    continue;
                }
                break;
            }

            // Align to block_align
            size_t chunkSize = std::min(available, size_t(8192));
            int blockAlign = m_codecCtx->block_align;
            if (blockAlign > 0) {
                chunkSize = (chunkSize / blockAlign) * blockAlign;
            }
            if (chunkSize == 0) {
                break;  // Not enough data for a complete frame
            }

            m_packet->data = const_cast<uint8_t*>(
                m_inputBuffer.data() + m_inputPos);
            m_packet->size = static_cast<int>(chunkSize);
            m_inputPos += chunkSize;

            int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
            if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                char errbuf[128];
                av_strerror(sendRet, errbuf, sizeof(errbuf));
                LOG_WARN("[FFmpeg] Send packet error: " << errbuf);
            }
        }
    }

    // Copy available output frames
    if (!m_formatReady || m_format.channels == 0) return 0;

    size_t channels = m_format.channels;
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

// ============================================================================
// Cleanup
// ============================================================================

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

    // Mode A cleanup
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        // avformat_close_input frees m_fmtCtx
    }
    if (m_avioCtx) {
        // Don't free m_avioBuf — avio_context_free handles it
        avio_context_free(&m_avioCtx);
        m_avioBuf = nullptr;
    } else if (m_avioBuf) {
        av_free(m_avioBuf);
        m_avioBuf = nullptr;
    }

    m_fmtCtx = nullptr;
    m_audioStreamIdx = -1;
    m_initialized = false;
}
