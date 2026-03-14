/**
 * @file FfmpegDecoder.cpp
 * @brief Audio decoder using FFmpeg's libavcodec (parser-based)
 *
 * Parser-based approach — no avformat, no AVIO, no probing:
 * 1. Codec is known from Slimproto format code (FLAC='f', MP3='m', etc.)
 * 2. av_parser_parse2() extracts codec frames from the input stream
 * 3. avcodec_send_packet() / avcodec_receive_frame() decodes frames
 * 4. Output converted to S32_LE interleaved MSB-aligned
 *
 * This is much more robust for push-based streaming than the avformat
 * approach, since there's no synchronous probing step.
 */

#include "FfmpegDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

AVCodecID FfmpegDecoder::formatCodeToCodecId(char code) {
    switch (code) {
        case 'f': return AV_CODEC_ID_FLAC;
        case 'm': return AV_CODEC_ID_MP3;
        case 'a': return AV_CODEC_ID_AAC;
        case 'o': return AV_CODEC_ID_VORBIS;
        case 'l': return AV_CODEC_ID_ALAC;
        case 'p': return AV_CODEC_ID_PCM_S16LE;  // Will be refined by setRawPcmFormat
        default:  return AV_CODEC_ID_NONE;
    }
}

FfmpegDecoder::FfmpegDecoder(char formatCode)
    : m_formatCode(formatCode) {
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
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
    m_initialized = false;
}

bool FfmpegDecoder::initDecoder() {
    AVCodecID codecId = formatCodeToCodecId(m_formatCode);

    // Refine PCM codec based on raw format hint
    if (m_formatCode == 'p' && m_rawPcmConfigured) {
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

    // Create parser (may be null for some codecs like raw PCM — that's OK)
    m_parser = av_parser_init(codec->id);
    if (m_formatCode == 'p' && m_parser) {
        // Raw PCM must not use a parser — we handle block_align
        // alignment ourselves.  Some FFmpeg versions provide a PCM
        // parser that splits data without respecting block_align,
        // producing 2-byte or 4-byte packets rejected by the decoder.
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
    if (!m_parser && m_formatCode != 'p') {
        LOG_WARN("[FFmpeg] No parser for " << codec->name
                 << " — will send raw packets");
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("[FFmpeg] Failed to allocate codec context");
        m_error = true;
        return false;
    }

    // For raw PCM with known format, set codec parameters
    if (m_formatCode == 'p' && m_rawPcmConfigured) {
        m_codecCtx->sample_rate = static_cast<int>(m_rawSampleRate);
        AVChannelLayout layout = {};
        av_channel_layout_default(&layout, static_cast<int>(m_rawChannels));
        av_channel_layout_copy(&m_codecCtx->ch_layout, &layout);
        av_channel_layout_uninit(&layout);
    }

    // Request S32 output for FLAC (gives MSB-aligned 24-bit in S32 container)
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
             << " (format code '" << m_formatCode << "')");

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

size_t FfmpegDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init on first call
    if (!m_initialized) {
        if (!initDecoder()) {
            return 0;
        }
    }

    // Parse and decode loop
    while (true) {
        // First, try to receive already-decoded frames
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0) {
            // Detect format from first decoded frame
            if (!m_formatReady) {
                int bitsPerRawSample = m_codecCtx->bits_per_raw_sample;
                if (bitsPerRawSample == 0) {
                    switch (m_codecCtx->sample_fmt) {
                        case AV_SAMPLE_FMT_S16:
                        case AV_SAMPLE_FMT_S16P:
                            bitsPerRawSample = 16; break;
                        case AV_SAMPLE_FMT_S32:
                        case AV_SAMPLE_FMT_S32P:
                            bitsPerRawSample = 24; break;
                        case AV_SAMPLE_FMT_FLT:
                        case AV_SAMPLE_FMT_FLTP:
                            bitsPerRawSample = 32; break;
                        default:
                            bitsPerRawSample = 16; break;
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

            // Check if we have enough output
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
            // Real error
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_WARN("[FFmpeg] Decode error: " << errbuf);
            break;
        }

        // EAGAIN: decoder needs more packets — parse input data
        size_t available = m_inputBuffer.size() - m_inputPos;
        if (available == 0) {
            if (m_eof) {
                // Flush parser first — it may have buffered the last
                // partial FLAC frame. Without this, the last few
                // samples of the stream are lost → click at gapless
                // track transitions.
                if (m_parser && !m_parserFlushed) {
                    uint8_t* outData = nullptr;
                    int outSize = 0;
                    av_parser_parse2(m_parser, m_codecCtx,
                                     &outData, &outSize,
                                     nullptr, 0,
                                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    m_parserFlushed = true;
                    if (outSize > 0) {
                        m_packet->data = outData;
                        m_packet->size = outSize;
                        avcodec_send_packet(m_codecCtx, m_packet);
                        continue;
                    }
                }
                // Flush decoder
                avcodec_send_packet(m_codecCtx, nullptr);
                continue;
            }
            break;  // No more data to parse, return what we have
        }

        if (m_parser) {
            // Use parser to extract codec frames
            const uint8_t* inData = m_inputBuffer.data() + m_inputPos;
            int inSize = static_cast<int>(std::min(available, size_t(65536)));

            uint8_t* outData = nullptr;
            int outSize = 0;

            int consumed = av_parser_parse2(
                m_parser, m_codecCtx,
                &outData, &outSize,
                inData, inSize,
                AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

            if (consumed > 0) {
                m_inputPos += static_cast<size_t>(consumed);

                // Compact input buffer periodically
                if (m_inputPos > 65536) {
                    m_inputBuffer.erase(m_inputBuffer.begin(),
                                         m_inputBuffer.begin() + m_inputPos);
                    m_inputPos = 0;
                }
            }

            if (outSize > 0) {
                // Got a complete frame — send to decoder
                m_packet->data = outData;
                m_packet->size = outSize;
                int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
                if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
                    char errbuf[128];
                    av_strerror(sendRet, errbuf, sizeof(errbuf));
                    LOG_WARN("[FFmpeg] Send packet error: " << errbuf);
                }
                // Don't unref — packet data points into parser's buffer
            }

            if (consumed == 0 && outSize == 0) {
                // Parser needs more data
                break;
            }
        } else {
            // No parser (raw PCM) — send data directly as a packet
            // Align to block_align to avoid sending partial frames
            // (e.g. pcm_s24le stereo: block_align=6, 8192%6=2 → 2-byte
            // remainder would be rejected by FFmpeg)
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
