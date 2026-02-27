/**
 * @file AacDecoder.cpp
 * @brief AAC stream decoder implementation using fdk-aac
 *
 * Key design: fdk-aac with ADTS transport for internet radio streams.
 * - aacDecoder_Fill() feeds encoded AAC data to the decoder
 * - aacDecoder_DecodeFrame() decodes one AAC frame at a time
 * - Output is INT_PCM (typically int16_t), converted to S32_LE MSB-aligned
 *
 * Handles:
 * - HE-AAC v2 with SBR (Spectral Band Replication) and PS (Parametric Stereo)
 * - ADTS sync errors (automatic resync, important for radio streams)
 * - Sample rate changes due to SBR (uses output sampleRate, not core rate)
 */

#include "AacDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

AacDecoder::AacDecoder() {
    m_handle = aacDecoder_Open(TT_MP4_ADTS, 1 /* nrOfLayers */);
    if (!m_handle) {
        LOG_ERROR("[AAC] Failed to open decoder");
        m_error = true;
        return;
    }

    // Enable SBR and PS for HE-AAC streams
    aacDecoder_SetParam(m_handle, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);

    m_inputBuffer.reserve(65536);
    m_outputBuffer.reserve(16384);
    m_decodeBuf.resize(2048 * 2);  // Max frame size * max channels
}

AacDecoder::~AacDecoder() {
    if (m_handle) {
        aacDecoder_Close(m_handle);
    }
}

size_t AacDecoder::feed(const uint8_t* data, size_t len) {
    m_inputBuffer.insert(m_inputBuffer.end(), data, data + len);
    return len;
}

void AacDecoder::setEof() {
    m_eof = true;
}

size_t AacDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    size_t channels = m_formatReady ? m_format.channels : 2;
    size_t outputFrames = (m_outputBuffer.size() - m_outputPos) / std::max(channels, size_t(1));

    while (outputFrames < maxFrames) {
        size_t available = m_inputBuffer.size() - m_inputPos;
        if (available == 0 && !m_eof) break;
        if (available == 0 && m_eof) {
            m_finished = true;
            break;
        }

        // Fill decoder with available data
        UCHAR* inBuf = m_inputBuffer.data() + m_inputPos;
        UINT bytesAvail = static_cast<UINT>(available);
        UINT bytesValid = bytesAvail;
        UCHAR* bufList[1] = { inBuf };
        UINT sizeList[1] = { bytesAvail };

        aacDecoder_Fill(m_handle, bufList, sizeList, &bytesValid);
        m_inputPos += (bytesAvail - bytesValid);

        // Compact input buffer periodically
        if (m_inputPos > 32768) {
            m_inputBuffer.erase(m_inputBuffer.begin(),
                                 m_inputBuffer.begin() + m_inputPos);
            m_inputPos = 0;
        }

        // Decode one frame
        AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(
            m_handle,
            m_decodeBuf.data(),
            static_cast<INT>(m_decodeBuf.size()),
            0 /* flags */
        );

        if (err == AAC_DEC_OK) {
            CStreamInfo* info = aacDecoder_GetStreamInfo(m_handle);
            if (!info || info->frameSize <= 0) continue;

            if (!m_formatReady) {
                // Use sampleRate (output rate after SBR) not aacSampleRate (core)
                m_format.sampleRate = static_cast<uint32_t>(info->sampleRate);
                m_format.channels = static_cast<uint32_t>(info->numChannels);
                m_format.bitDepth = 16;
                m_format.totalSamples = 0;
                m_shift = 16;  // 32 - 16
                m_formatReady = true;
                channels = m_format.channels;

                LOG_INFO("[AAC] Format: " << info->sampleRate << " Hz, "
                         << info->numChannels << " ch"
                         << (info->extAot == AOT_SBR ? " (HE-AAC)" : "")
                         << (info->extAot == AOT_PS ? " (HE-AAC v2)" : ""));
            }

            // Check for format changes (SBR activation, etc.)
            if (static_cast<uint32_t>(info->sampleRate) != m_format.sampleRate ||
                static_cast<uint32_t>(info->numChannels) != m_format.channels) {
                m_format.sampleRate = static_cast<uint32_t>(info->sampleRate);
                m_format.channels = static_cast<uint32_t>(info->numChannels);
                channels = m_format.channels;
                LOG_INFO("[AAC] Format change: " << info->sampleRate << " Hz, "
                         << info->numChannels << " ch");
            }

            // Convert INT_PCM to S32_LE MSB-aligned
            int numSamples = info->frameSize * info->numChannels;
            for (int i = 0; i < numSamples; i++) {
                m_outputBuffer.push_back(
                    static_cast<int32_t>(m_decodeBuf[i]) << m_shift);
            }

        } else if (err == AAC_DEC_NOT_ENOUGH_BITS) {
            // Need more data
            if (m_eof) {
                m_finished = true;
            }
            break;

        } else if (err == AAC_DEC_TRANSPORT_SYNC_ERROR) {
            // ADTS sync lost — normal for radio, try to resync
            LOG_DEBUG("[AAC] Transport sync error (resyncing)");
            continue;

        } else {
            // Other errors
            LOG_WARN("[AAC] Decode error: 0x" << std::hex << err << std::dec);
            if (m_eof) {
                m_finished = true;
                break;
            }
            // Try to continue for radio streams
            continue;
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

void AacDecoder::flush() {
    if (m_handle) {
        aacDecoder_Close(m_handle);
    }
    m_handle = aacDecoder_Open(TT_MP4_ADTS, 1);
    if (m_handle) {
        aacDecoder_SetParam(m_handle, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
    }

    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_decodeBuf.assign(m_decodeBuf.size(), 0);
    m_format = {};
    m_formatReady = false;
    m_shift = 0;
    m_error = (m_handle == nullptr);
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
}
