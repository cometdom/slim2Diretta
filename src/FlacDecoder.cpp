/**
 * @file FlacDecoder.cpp
 * @brief FLAC stream decoder implementation using libFLAC
 *
 * Key design: two-phase decoding to handle large metadata blocks (album art).
 *
 * Phase 1 (metadata): Uses process_until_end_of_metadata(). If ABORT happens
 * (not enough data for all metadata), the decoder is deleted and recreated
 * on the next attempt with more accumulated data.
 *
 * Phase 2 (audio): Uses process_single() per frame. On ABORT (incomplete frame),
 * we rollback to the last confirmed frame boundary using get_decode_position()
 * and flush(). This avoids losing the read-ahead bytes that libFLAC keeps in
 * its internal buffer — we only compact up to the confirmed position, so those
 * bytes remain in our buffer and get re-provided on the next call.
 */

#include "FlacDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>

FlacDecoder::FlacDecoder() {
    m_inputBuffer.reserve(131072);  // 128KB — enough for most metadata blocks
    m_outputBuffer.reserve(16384);
}

FlacDecoder::~FlacDecoder() {
    if (m_decoder) {
        FLAC__stream_decoder_delete(m_decoder);
    }
}

bool FlacDecoder::initDecoder() {
    m_decoder = FLAC__stream_decoder_new();
    if (!m_decoder) {
        LOG_ERROR("[FLAC] Failed to create decoder");
        m_error = true;
        return false;
    }

    FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
        m_decoder,
        readCallback,
        nullptr,        // seek
        tellCallback,   // tell — needed for get_decode_position()
        nullptr,        // length
        nullptr,        // eof
        writeCallback,
        metadataCallback,
        errorCallback,
        this
    );

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        LOG_ERROR("[FLAC] Init failed: " << FLAC__StreamDecoderInitStatusString[status]);
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = nullptr;
        m_error = true;
        return false;
    }

    m_initialized = true;
    return true;
}

size_t FlacDecoder::feed(const uint8_t* data, size_t len) {
    m_inputBuffer.insert(m_inputBuffer.end(), data, data + len);
    return len;
}

void FlacDecoder::setEof() {
    m_eof = true;
}

size_t FlacDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init on first decode attempt
    if (!m_initialized) {
        if (!initDecoder()) return 0;
    }

    // ================================================================
    // Phase 1: Process all metadata blocks
    // ================================================================
    // FLAC files can have large metadata (album art = 100KB+).
    // If ABORT happens during metadata (not enough data), we delete
    // the decoder entirely and recreate it on the next call with more
    // accumulated data. The input buffer is NOT compacted during this
    // phase, so all previously fed data is available for the retry.

    if (!m_metadataDone) {
        size_t savedPos = m_inputPos;

        if (!FLAC__stream_decoder_process_until_end_of_metadata(m_decoder)) {
            FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);

            if (state == FLAC__STREAM_DECODER_ABORTED) {
                // Not enough data for all metadata — need more input
                m_inputPos = savedPos;  // Rollback: keep all data
                FLAC__stream_decoder_delete(m_decoder);
                m_decoder = nullptr;
                m_initialized = false;
                m_metadataRetries++;
                // Log first attempt and then only every 50th to avoid spam
                // (Qobuz streams with large album art can need 100+ retries)
                if (m_metadataRetries == 1) {
                    LOG_DEBUG("[FLAC] Metadata incomplete, need more data ("
                              << m_inputBuffer.size() << " bytes buffered)");
                } else if (m_metadataRetries % 50 == 0) {
                    LOG_DEBUG("[FLAC] Metadata still incomplete after "
                              << m_metadataRetries << " retries ("
                              << m_inputBuffer.size() << " bytes buffered)");
                }
                return 0;
            }

            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                m_finished = true;
                return 0;
            }

            LOG_ERROR("[FLAC] Metadata processing failed: "
                      << FLAC__StreamDecoderStateString[state]);
            m_error = true;
            return 0;
        }

        m_metadataDone = true;
        if (m_metadataRetries > 0) {
            LOG_DEBUG("[FLAC] Metadata complete after " << m_metadataRetries
                      << " retries (" << m_inputBuffer.size() << " bytes buffered)");
        } else {
            LOG_DEBUG("[FLAC] Metadata complete, starting audio decode");
        }

        // Use get_decode_position to find exact metadata/audio boundary.
        // This excludes read-ahead bytes in libFLAC's internal buffer,
        // so those bytes stay in our buffer for the audio phase.
        FLAC__uint64 absPos;
        if (FLAC__stream_decoder_get_decode_position(m_decoder, &absPos)) {
            size_t audioStart = static_cast<size_t>(absPos - m_tellOffset);
            if (audioStart > 0 && audioStart <= m_inputBuffer.size()) {
                m_inputBuffer.erase(m_inputBuffer.begin(),
                                    m_inputBuffer.begin() + audioStart);
                m_inputPos -= audioStart;
                m_tellOffset += audioStart;
            }
        } else {
            // Fallback: compact to m_inputPos (may lose read-ahead on first ABORT)
            if (m_inputPos > 0) {
                m_tellOffset += m_inputPos;
                m_inputBuffer.erase(m_inputBuffer.begin(),
                                    m_inputBuffer.begin() + m_inputPos);
                m_inputPos = 0;
            }
        }
        m_confirmedAbsolutePos = m_tellOffset;
    }

    // ================================================================
    // Phase 2: Decode audio frames
    // ================================================================
    // On ABORT (incomplete frame), we rollback m_inputPos to the last
    // confirmed frame boundary (from get_decode_position), not to
    // savedInputPos. This ensures read-ahead bytes from the previous
    // successful frame are re-provided to libFLAC after flush().

    size_t outputAvailable = (m_outputBuffer.size() - m_outputPos) /
                              std::max(m_format.channels, 1u);

    while (outputAvailable < maxFrames) {
        size_t inputAvailable = m_inputBuffer.size() - m_inputPos;
        if (inputAvailable == 0 && !m_eof) break;

        if (!FLAC__stream_decoder_process_single(m_decoder)) {
            FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);

            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                m_finished = true;
                break;
            }

            if (state == FLAC__STREAM_DECODER_ABORTED) {
                // Rollback to last confirmed frame boundary
                size_t confirmedBufPos = static_cast<size_t>(
                    m_confirmedAbsolutePos - m_tellOffset);
                m_inputPos = confirmedBufPos;

                if (!m_eof && !m_error) {
                    FLAC__stream_decoder_flush(m_decoder);
                    break;  // Wait for more input
                }
                // At EOF with incomplete frame — stream truncated
                LOG_WARN("[FLAC] Stream ended with incomplete frame");
                m_finished = true;
                break;
            }

            // Other error
            LOG_ERROR("[FLAC] Decoder error state: "
                      << FLAC__StreamDecoderStateString[state]);
            m_error = true;
            break;
        }

        // Check for end of stream after successful process
        FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);
        if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
            m_finished = true;
            break;
        }

        // Update confirmed position after successful frame decode
        FLAC__uint64 absPos;
        if (FLAC__stream_decoder_get_decode_position(m_decoder, &absPos)) {
            m_confirmedAbsolutePos = absPos;
        }

        outputAvailable = (m_outputBuffer.size() - m_outputPos) /
                           std::max(m_format.channels, 1u);
    }

    // Compact input buffer: only remove confirmed-consumed bytes.
    // Read-ahead bytes (between confirmed pos and m_inputPos) stay in the buffer.
    size_t confirmedBufPos = static_cast<size_t>(m_confirmedAbsolutePos - m_tellOffset);
    if (confirmedBufPos > 0) {
        m_inputBuffer.erase(m_inputBuffer.begin(),
                            m_inputBuffer.begin() + confirmedBufPos);
        m_inputPos -= confirmedBufPos;
        m_tellOffset += confirmedBufPos;
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

void FlacDecoder::flush() {
    if (m_decoder) {
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = nullptr;
    }
    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_shift = 0;
    m_tellOffset = 0;
    m_confirmedAbsolutePos = 0;
    m_initialized = false;
    m_metadataDone = false;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
    m_metadataRetries = 0;
}

// ============================================
// libFLAC Callbacks
// ============================================

FLAC__StreamDecoderReadStatus FlacDecoder::readCallback(
    const FLAC__StreamDecoder*, FLAC__byte buffer[],
    size_t* bytes, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    size_t available = self->m_inputBuffer.size() - self->m_inputPos;

    if (available == 0) {
        *bytes = 0;
        if (self->m_eof) {
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    size_t toRead = std::min(available, *bytes);
    std::memcpy(buffer, self->m_inputBuffer.data() + self->m_inputPos, toRead);
    self->m_inputPos += toRead;
    *bytes = toRead;

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus FlacDecoder::writeCallback(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    uint32_t channels = frame->header.channels;
    uint32_t blocksize = frame->header.blocksize;
    int shift = self->m_shift;

    // Reserve space for interleaved output
    size_t prevSize = self->m_outputBuffer.size();
    self->m_outputBuffer.resize(prevSize + blocksize * channels);
    int32_t* dst = self->m_outputBuffer.data() + prevSize;

    // Interleave and MSB-align samples
    for (uint32_t i = 0; i < blocksize; i++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            *dst++ = buffer[ch][i] << shift;
        }
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacDecoder::metadataCallback(
    const FLAC__StreamDecoder*,
    const FLAC__StreamMetadata* metadata, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& info = metadata->data.stream_info;
        self->m_format.sampleRate = info.sample_rate;
        self->m_format.bitDepth = info.bits_per_sample;
        self->m_format.channels = info.channels;
        self->m_format.totalSamples = info.total_samples;

        self->m_shift = 32 - static_cast<int>(info.bits_per_sample);
        self->m_formatReady = true;

        LOG_INFO("[FLAC] Format: " << info.sample_rate << " Hz, "
                 << info.bits_per_sample << "-bit, "
                 << info.channels << " ch"
                 << (info.total_samples > 0
                     ? ", " + std::to_string(info.total_samples) + " samples"
                     : ""));
    }
}

void FlacDecoder::errorCallback(
    const FLAC__StreamDecoder*,
    FLAC__StreamDecoderErrorStatus status, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    switch (status) {
        case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
            // Normal during sync acquisition — silently ignore
            return;

        case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
        case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:
            // Recoverable — decoder will search for next valid frame
            LOG_DEBUG("[FLAC] " << FLAC__StreamDecoderErrorStatusString[status]);
            return;

        default:
            // UNPARSEABLE_STREAM or unknown — fatal
            LOG_ERROR("[FLAC] Decode error: " << FLAC__StreamDecoderErrorStatusString[status]);
            self->m_error = true;
            break;
    }
}

FLAC__StreamDecoderTellStatus FlacDecoder::tellCallback(
    const FLAC__StreamDecoder*, FLAC__uint64* absolute_byte_offset,
    void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);
    *absolute_byte_offset = self->m_tellOffset + self->m_inputPos;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
