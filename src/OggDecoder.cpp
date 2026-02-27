/**
 * @file OggDecoder.cpp
 * @brief Ogg Vorbis stream decoder implementation using libvorbisfile
 *
 * Key design: libvorbisfile with custom non-seekable callbacks.
 * - readCallback() pulls data from the internal input buffer
 * - ov_read() decodes Vorbis audio to 16-bit PCM
 * - Output is converted to S32_LE MSB-aligned (shift left 16)
 *
 * Handles:
 * - OV_HOLE: gaps in data (normal for radio streams, logged and skipped)
 * - Chained streams: format may change between chains (re-read ov_info)
 * - Streaming: no seek/tell callbacks (non-seekable stream)
 */

#include "OggDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>
#include <cerrno>

OggDecoder::OggDecoder() {
    m_inputBuffer.reserve(65536);
    m_outputBuffer.reserve(16384);
    std::memset(&m_vf, 0, sizeof(m_vf));
}

OggDecoder::~OggDecoder() {
    if (m_vfOpen) {
        ov_clear(&m_vf);
    }
}

size_t OggDecoder::feed(const uint8_t* data, size_t len) {
    m_inputBuffer.insert(m_inputBuffer.end(), data, data + len);
    return len;
}

void OggDecoder::setEof() {
    m_eof = true;
}

size_t OggDecoder::readCallback(void* ptr, size_t size, size_t nmemb, void* datasource) {
    auto* self = static_cast<OggDecoder*>(datasource);

    size_t available = self->m_inputBuffer.size() - self->m_inputPos;
    size_t requested = size * nmemb;

    if (available == 0) {
        if (self->m_eof) {
            return 0;  // EOF
        }
        // No data yet, not EOF — signal "would block"
        errno = EAGAIN;
        return 0;
    }

    size_t toRead = std::min(available, requested);
    std::memcpy(ptr, self->m_inputBuffer.data() + self->m_inputPos, toRead);
    self->m_inputPos += toRead;

    // Compact input buffer periodically
    if (self->m_inputPos > 32768) {
        self->m_inputBuffer.erase(self->m_inputBuffer.begin(),
                                   self->m_inputBuffer.begin() + self->m_inputPos);
        self->m_inputPos = 0;
    }

    return toRead / size;
}

size_t OggDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init: open vorbisfile on first call with enough data
    if (!m_initialized) {
        size_t available = m_inputBuffer.size() - m_inputPos;
        if (available < 4096 && !m_eof) {
            return 0;  // Wait for more data before attempting init
        }

        ov_callbacks cb;
        cb.read_func = readCallback;
        cb.seek_func = nullptr;   // Non-seekable stream
        cb.close_func = nullptr;
        cb.tell_func = nullptr;

        int ret = ov_open_callbacks(this, &m_vf, nullptr, 0, cb);
        if (ret < 0) {
            if (ret == OV_EREAD && !m_eof) {
                // Need more data for init
                return 0;
            }
            LOG_ERROR("[OGG] Failed to open stream (error " << ret << ")");
            m_error = true;
            return 0;
        }

        m_vfOpen = true;
        m_initialized = true;

        // Read initial format
        vorbis_info* vi = ov_info(&m_vf, -1);
        if (vi) {
            m_format.sampleRate = static_cast<uint32_t>(vi->rate);
            m_format.channels = static_cast<uint32_t>(vi->channels);
            m_format.bitDepth = 16;
            m_format.totalSamples = 0;  // Unknown for streams
            m_formatReady = true;
            m_currentBitstream = 0;

            LOG_INFO("[OGG] Format: " << vi->rate << " Hz, " << vi->channels << " ch");
        }
    }

    // Decode into output buffer
    size_t channels = m_formatReady ? m_format.channels : 2;
    size_t outputFrames = (m_outputBuffer.size() - m_outputPos) / std::max(channels, size_t(1));

    while (outputFrames < maxFrames) {
        // Check if we have data to decode
        size_t available = m_inputBuffer.size() - m_inputPos;
        if (available == 0 && !m_eof) break;

        char pcmBuf[4096];
        int bitstream = 0;
        long ret = ov_read(&m_vf, pcmBuf, sizeof(pcmBuf),
                           0 /* little-endian */,
                           2 /* 16-bit */,
                           1 /* signed */,
                           &bitstream);

        if (ret > 0) {
            // Check for chained stream (format change)
            if (bitstream != m_currentBitstream) {
                m_currentBitstream = bitstream;
                vorbis_info* vi = ov_info(&m_vf, -1);
                if (vi) {
                    m_format.sampleRate = static_cast<uint32_t>(vi->rate);
                    m_format.channels = static_cast<uint32_t>(vi->channels);
                    channels = m_format.channels;
                    LOG_INFO("[OGG] Chain change: " << vi->rate << " Hz, "
                             << vi->channels << " ch");
                }
            }

            // Convert 16-bit signed to S32_LE MSB-aligned
            size_t numSamples = static_cast<size_t>(ret) / 2;  // 2 bytes per sample
            const int16_t* src = reinterpret_cast<const int16_t*>(pcmBuf);
            for (size_t i = 0; i < numSamples; i++) {
                m_outputBuffer.push_back(static_cast<int32_t>(src[i]) << 16);
            }
        } else if (ret == 0) {
            // EOF or need more data
            if (m_eof) {
                m_finished = true;
            }
            break;
        } else if (ret == OV_HOLE) {
            // Gap in data — normal for radio, continue
            LOG_DEBUG("[OGG] Data gap (OV_HOLE), continuing");
            continue;
        } else if (ret == OV_EBADLINK) {
            LOG_WARN("[OGG] Bad link in stream, attempting recovery");
            continue;
        } else if (ret == OV_EINVAL) {
            LOG_ERROR("[OGG] Invalid stream state");
            m_error = true;
            break;
        } else {
            // OV_EREAD — read callback returned error
            if (!m_eof) {
                break;  // Need more data
            }
            m_finished = true;
            break;
        }

        outputFrames = (m_outputBuffer.size() - m_outputPos) / std::max(channels, size_t(1));
    }

    // Copy available output frames
    if (!m_formatReady || m_format.channels == 0) return 0;

    size_t framesAvailable = (m_outputBuffer.size() - m_outputPos) / m_format.channels;
    size_t framesToCopy = std::min(framesAvailable, maxFrames);

    if (framesToCopy > 0) {
        size_t samplesToCopy = framesToCopy * m_format.channels;
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

void OggDecoder::flush() {
    if (m_vfOpen) {
        ov_clear(&m_vf);
        m_vfOpen = false;
    }
    std::memset(&m_vf, 0, sizeof(m_vf));
    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_initialized = false;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
    m_currentBitstream = -1;
}
