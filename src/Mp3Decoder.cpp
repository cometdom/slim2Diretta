/**
 * @file Mp3Decoder.cpp
 * @brief MP3 stream decoder implementation using libmpg123
 *
 * Key design: mpg123 feed mode for streaming.
 * - mpg123_feed() pushes encoded data into mpg123's internal buffer
 * - mpg123_read() pulls decoded PCM samples
 * - MPG123_NEW_FORMAT signals format detection (sample rate, channels)
 * - MPG123_NEED_MORE signals need for more input data
 * - Error recovery is automatic (mpg123 resyncs on corrupted frames)
 *
 * Output: MPG123_ENC_SIGNED_32 — full-scale 32-bit signed, already MSB-aligned.
 */

#include "Mp3Decoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

std::once_flag Mp3Decoder::s_initFlag;

Mp3Decoder::Mp3Decoder() {
    // Global mpg123 init (thread-safe, once)
    std::call_once(s_initFlag, [] {
        mpg123_init();
    });

    m_outputBuffer.reserve(16384);
}

Mp3Decoder::~Mp3Decoder() {
    if (m_handle) {
        mpg123_close(m_handle);
        mpg123_delete(m_handle);
    }
}

bool Mp3Decoder::initHandle() {
    int err;
    m_handle = mpg123_new(nullptr, &err);
    if (!m_handle) {
        LOG_ERROR("[MP3] Failed to create decoder: " << mpg123_plain_strerror(err));
        m_error = true;
        return false;
    }

    // Open in feed mode (streaming, no file)
    if (mpg123_open_feed(m_handle) != MPG123_OK) {
        LOG_ERROR("[MP3] Failed to open feed: " << mpg123_strerror(m_handle));
        mpg123_delete(m_handle);
        m_handle = nullptr;
        m_error = true;
        return false;
    }

    // Clear all format constraints, then set our desired output
    mpg123_format_none(m_handle);

    // Accept any sample rate, mono or stereo, 32-bit signed output
    // mpg123 will scale to full 32-bit range (MSB-aligned)
    const long rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000,
        32000, 44100, 48000
    };
    for (long rate : rates) {
        mpg123_format(m_handle, rate, MPG123_MONO | MPG123_STEREO,
                      MPG123_ENC_SIGNED_32);
    }

    m_initialized = true;
    return true;
}

size_t Mp3Decoder::feed(const uint8_t* data, size_t len) {
    if (!m_initialized) {
        if (!initHandle()) return 0;
    }

    int ret = mpg123_feed(m_handle, data, len);
    if (ret != MPG123_OK) {
        LOG_ERROR("[MP3] Feed error: " << mpg123_strerror(m_handle));
        return 0;
    }
    return len;
}

void Mp3Decoder::setEof() {
    m_eof = true;
}

size_t Mp3Decoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    if (!m_initialized) {
        if (!initHandle()) return 0;
    }

    // Decode into output buffer
    size_t channels = m_formatReady ? m_format.channels : 2;
    size_t outputFrames = (m_outputBuffer.size() - m_outputPos) / std::max(channels, size_t(1));

    while (outputFrames < maxFrames) {
        // Temp buffer for mpg123 output (max ~1152 frames * 2 ch * 4 bytes)
        unsigned char tmpBuf[1152 * 2 * sizeof(int32_t)];
        size_t done = 0;

        int ret = mpg123_read(m_handle, tmpBuf, sizeof(tmpBuf), &done);

        if (ret == MPG123_NEW_FORMAT) {
            long rate;
            int ch, encoding;
            mpg123_getformat(m_handle, &rate, &ch, &encoding);

            m_format.sampleRate = static_cast<uint32_t>(rate);
            m_format.channels = static_cast<uint32_t>(ch);
            m_format.bitDepth = 32;  // MPG123_ENC_SIGNED_32 outputs full-scale 32-bit
            m_format.totalSamples = 0;  // Unknown for streams
            m_formatReady = true;
            channels = static_cast<size_t>(ch);

            LOG_INFO("[MP3] Format: " << rate << " Hz, " << ch << " ch");
        }

        if (done > 0) {
            // mpg123 outputs int32_t in native byte order when using MPG123_ENC_SIGNED_32
            // Already full-scale 32-bit, no shift needed
            size_t samples = done / sizeof(int32_t);
            const int32_t* src = reinterpret_cast<const int32_t*>(tmpBuf);
            m_outputBuffer.insert(m_outputBuffer.end(), src, src + samples);
        }

        if (ret == MPG123_NEED_MORE) {
            if (m_eof) {
                m_finished = true;
            }
            break;
        }

        if (ret == MPG123_DONE) {
            m_finished = true;
            break;
        }

        if (ret == MPG123_ERR) {
            // Log but continue — resync for radio streams
            LOG_DEBUG("[MP3] Decode error (resyncing): " << mpg123_strerror(m_handle));
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

void Mp3Decoder::flush() {
    if (m_handle) {
        mpg123_close(m_handle);
        mpg123_delete(m_handle);
        m_handle = nullptr;
    }
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_initialized = false;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
}
