/**
 * @file PcmDecoder.cpp
 * @brief PCM decoder for WAV (RIFF) and AIFF containers
 */

#include "PcmDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <cmath>
#include <algorithm>

// Minimum header sizes
static constexpr size_t WAV_MIN_HEADER = 44;   // RIFF(12) + fmt(24) + data(8)
static constexpr size_t AIFF_MIN_HEADER = 46;  // FORM(12) + COMM(26) + SSND(8)

// Read helpers (little-endian)
static uint16_t readLE16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readLE32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Read helpers (big-endian)
static uint16_t readBE16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static uint32_t readBE32(const uint8_t* p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

PcmDecoder::PcmDecoder() {
    m_headerBuf.reserve(256);
    m_dataBuf.reserve(32768);
}

size_t PcmDecoder::feed(const uint8_t* data, size_t len) {
    if (m_state == State::DETECT || m_state == State::PARSE_WAV ||
        m_state == State::PARSE_AIFF) {
        m_headerBuf.insert(m_headerBuf.end(), data, data + len);
    } else if (m_state == State::DATA) {
        m_dataBuf.insert(m_dataBuf.end(), data, data + len);
    }
    return len;
}

void PcmDecoder::setEof() {
    m_eof = true;
}

size_t PcmDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Try to detect/parse container
    if (m_state == State::DETECT) {
        if (!detectContainer()) return 0;
    }

    if (m_state == State::PARSE_WAV) {
        if (!parseWavHeader()) return 0;
    } else if (m_state == State::PARSE_AIFF) {
        if (!parseAiffHeader()) return 0;
    }

    if (m_state != State::DATA) return 0;

    // Convert available PCM data to S32_LE
    uint32_t bytesPerSample = m_format.bitDepth / 8;
    uint32_t bytesPerFrame = bytesPerSample * m_format.channels;
    if (bytesPerFrame == 0) return 0;

    // Limit by available data and remaining chunk size
    size_t availBytes = m_dataBuf.size();
    if (m_dataRemaining > 0) {
        availBytes = std::min(availBytes, static_cast<size_t>(m_dataRemaining));
    }

    size_t framesAvail = availBytes / bytesPerFrame;
    size_t framesToConvert = std::min(framesAvail, maxFrames);
    if (framesToConvert == 0) {
        // Only finish when we know no more data will arrive:
        // - EOF signaled by caller (HTTP stream ended)
        // - All data chunk bytes consumed (m_dataRemaining reached 0 via subtraction)
        // Do NOT finish just because the buffer is temporarily empty —
        // more data may arrive from the next HTTP read.
        if (m_eof) {
            m_finished = true;
        }
        return 0;
    }

    size_t bytesToConvert = framesToConvert * bytesPerFrame;
    convertSamples(m_dataBuf.data(), out, bytesToConvert);

    // Consume from data buffer
    m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + bytesToConvert);
    if (m_dataRemaining > 0) {
        m_dataRemaining -= bytesToConvert;
        if (m_dataRemaining == 0) {
            m_finished = true;
        }
    }

    m_decodedSamples += framesToConvert;
    return framesToConvert;
}

void PcmDecoder::setRawPcmFormat(uint32_t sampleRate, uint32_t bitDepth,
                                  uint32_t channels, bool bigEndian) {
    m_format.sampleRate = sampleRate;
    m_format.bitDepth = bitDepth;
    m_format.channels = channels;
    m_format.totalSamples = 0;  // Unknown for raw streams
    m_bigEndian = bigEndian;
    m_shift = 32 - bitDepth;
    m_rawPcmConfigured = true;
}

void PcmDecoder::flush() {
    m_state = State::DETECT;
    m_headerBuf.clear();
    m_dataBuf.clear();
    m_format = {};
    m_formatReady = false;
    m_bigEndian = false;
    m_shift = 0;
    m_dataRemaining = 0;
    m_rawPcmConfigured = false;
    m_eof = false;
    m_error = false;
    m_finished = false;
    m_decodedSamples = 0;
}

bool PcmDecoder::detectContainer() {
    if (m_headerBuf.size() < 4) return false;

    if (std::memcmp(m_headerBuf.data(), "RIFF", 4) == 0) {
        m_state = State::PARSE_WAV;
        LOG_DEBUG("[PCM] WAV container detected");
        return true;
    }
    if (std::memcmp(m_headerBuf.data(), "FORM", 4) == 0) {
        m_state = State::PARSE_AIFF;
        LOG_DEBUG("[PCM] AIFF container detected");
        return true;
    }

    // No container — use raw PCM format from strm command (Roon, etc.)
    if (m_rawPcmConfigured) {
        m_formatReady = true;
        m_dataRemaining = 0;  // Unlimited (stream until EOF)
        // Move all accumulated data to data buffer (it's audio, not a header)
        m_dataBuf.insert(m_dataBuf.end(), m_headerBuf.begin(), m_headerBuf.end());
        m_headerBuf.clear();
        m_state = State::DATA;
        LOG_INFO("[PCM] Raw: " << m_format.sampleRate << " Hz, "
                 << m_format.bitDepth << "-bit, " << m_format.channels << " ch"
                 << (m_bigEndian ? " BE" : " LE"));
        return true;
    }

    LOG_ERROR("[PCM] Unknown container magic: 0x"
              << std::hex << (int)m_headerBuf[0] << (int)m_headerBuf[1]
              << (int)m_headerBuf[2] << (int)m_headerBuf[3] << std::dec);
    m_state = State::ERROR;
    m_error = true;
    return false;
}

bool PcmDecoder::parseWavHeader() {
    if (m_headerBuf.size() < WAV_MIN_HEADER) return false;

    const uint8_t* p = m_headerBuf.data();

    // RIFF header
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        LOG_ERROR("[PCM] Invalid WAV header");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    // Search for fmt and data chunks
    size_t pos = 12;
    bool foundFmt = false;
    bool foundData = false;
    size_t dataStart = 0;

    while (pos + 8 <= m_headerBuf.size()) {
        uint32_t chunkSize = readLE32(p + pos + 4);

        if (std::memcmp(p + pos, "fmt ", 4) == 0) {
            if (pos + 8 + chunkSize > m_headerBuf.size()) return false;  // Need more data

            uint16_t audioFormat = readLE16(p + pos + 8);
            bool isExtensible = (audioFormat == 0xFFFE);

            // WAVE_FORMAT_EXTENSIBLE: actual format in SubFormat GUID
            if (isExtensible) {
                if (chunkSize < 40) {
                    LOG_ERROR("[PCM] EXTENSIBLE fmt chunk too small: " << chunkSize);
                    m_state = State::ERROR;
                    m_error = true;
                    return false;
                }
                // SubFormat GUID first 2 bytes = actual format code
                audioFormat = readLE16(p + pos + 8 + 24);
            }

            if (audioFormat != 1 && audioFormat != 3) {  // 1=PCM, 3=IEEE float
                LOG_ERROR("[PCM] Unsupported WAV format: " << audioFormat);
                m_state = State::ERROR;
                m_error = true;
                return false;
            }

            m_format.channels = readLE16(p + pos + 10);
            m_format.sampleRate = readLE32(p + pos + 12);
            m_format.bitDepth = readLE16(p + pos + 22);

            // EXTENSIBLE: wValidBitsPerSample may differ from container size
            if (isExtensible) {
                uint16_t validBits = readLE16(p + pos + 8 + 18);
                if (validBits > 0) {
                    m_format.bitDepth = validBits;
                }
            }

            m_bigEndian = false;
            foundFmt = true;
        }
        else if (std::memcmp(p + pos, "data", 4) == 0) {
            m_dataRemaining = chunkSize;
            dataStart = pos + 8;
            foundData = true;
        }

        if (foundFmt && foundData) break;

        // Advance to next chunk (chunks are word-aligned)
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;  // Pad byte
    }

    if (!foundFmt || !foundData) return false;  // Need more header data

    m_shift = 32 - m_format.bitDepth;
    m_format.totalSamples = m_dataRemaining /
                             (m_format.bitDepth / 8 * m_format.channels);
    m_formatReady = true;

    LOG_INFO("[PCM] WAV: " << m_format.sampleRate << " Hz, "
             << m_format.bitDepth << "-bit, " << m_format.channels << " ch");

    // Move remaining header bytes (after data chunk start) to data buffer
    if (dataStart < m_headerBuf.size()) {
        m_dataBuf.insert(m_dataBuf.end(),
                         m_headerBuf.begin() + dataStart,
                         m_headerBuf.end());
    }
    m_headerBuf.clear();
    m_state = State::DATA;
    return true;
}

bool PcmDecoder::parseAiffHeader() {
    if (m_headerBuf.size() < AIFF_MIN_HEADER) return false;

    const uint8_t* p = m_headerBuf.data();

    // FORM header
    if (std::memcmp(p, "FORM", 4) != 0 ||
        (std::memcmp(p + 8, "AIFF", 4) != 0 && std::memcmp(p + 8, "AIFC", 4) != 0)) {
        LOG_ERROR("[PCM] Invalid AIFF header");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    // Search for COMM and SSND chunks
    size_t pos = 12;
    bool foundComm = false;
    bool foundSsnd = false;
    size_t dataStart = 0;

    while (pos + 8 <= m_headerBuf.size()) {
        uint32_t chunkSize = readBE32(p + pos + 4);

        if (std::memcmp(p + pos, "COMM", 4) == 0) {
            if (pos + 8 + chunkSize > m_headerBuf.size()) return false;

            m_format.channels = readBE16(p + pos + 8);
            uint32_t numFrames = readBE32(p + pos + 10);
            m_format.bitDepth = readBE16(p + pos + 14);
            m_format.sampleRate = extendedToUint32(p + pos + 16);
            m_format.totalSamples = numFrames;
            m_bigEndian = true;
            foundComm = true;
        }
        else if (std::memcmp(p + pos, "SSND", 4) == 0) {
            if (pos + 16 > m_headerBuf.size()) return false;

            uint32_t offset = readBE32(p + pos + 8);
            m_dataRemaining = chunkSize - 8;  // Subtract offset + blockSize fields
            dataStart = pos + 16 + offset;    // Skip SSND header + offset
            foundSsnd = true;
        }

        if (foundComm && foundSsnd) break;

        // Advance to next chunk (AIFF chunks are word-aligned)
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++;
    }

    if (!foundComm || !foundSsnd) return false;

    m_shift = 32 - m_format.bitDepth;
    m_formatReady = true;

    LOG_INFO("[PCM] AIFF: " << m_format.sampleRate << " Hz, "
             << m_format.bitDepth << "-bit, " << m_format.channels << " ch");

    // Move remaining data to data buffer
    if (dataStart < m_headerBuf.size()) {
        m_dataBuf.insert(m_dataBuf.end(),
                         m_headerBuf.begin() + dataStart,
                         m_headerBuf.end());
    }
    m_headerBuf.clear();
    m_state = State::DATA;
    return true;
}

size_t PcmDecoder::convertSamples(const uint8_t* src, int32_t* dst, size_t srcBytes) {
    uint32_t bytesPerSample = m_format.bitDepth / 8;
    size_t numSamples = srcBytes / bytesPerSample;

    if (m_bigEndian) {
        // Big-endian (AIFF)
        switch (bytesPerSample) {
            case 1:
                for (size_t i = 0; i < numSamples; i++) {
                    dst[i] = static_cast<int32_t>(static_cast<int8_t>(src[i])) << 24;
                }
                break;
            case 2:
                for (size_t i = 0; i < numSamples; i++) {
                    int16_t s = static_cast<int16_t>((src[i*2] << 8) | src[i*2+1]);
                    dst[i] = static_cast<int32_t>(s) << 16;
                }
                break;
            case 3:
                for (size_t i = 0; i < numSamples; i++) {
                    int32_t s = (src[i*3] << 24) | (src[i*3+1] << 16) | (src[i*3+2] << 8);
                    dst[i] = s;  // Already MSB-aligned
                }
                break;
            case 4:
                for (size_t i = 0; i < numSamples; i++) {
                    dst[i] = static_cast<int32_t>(
                        (src[i*4] << 24) | (src[i*4+1] << 16) |
                        (src[i*4+2] << 8) | src[i*4+3]);
                }
                break;
        }
    } else {
        // Little-endian (WAV)
        switch (bytesPerSample) {
            case 1:
                for (size_t i = 0; i < numSamples; i++) {
                    dst[i] = static_cast<int32_t>(static_cast<int8_t>(src[i])) << 24;
                }
                break;
            case 2:
                for (size_t i = 0; i < numSamples; i++) {
                    int16_t s = static_cast<int16_t>(src[i*2] | (src[i*2+1] << 8));
                    dst[i] = static_cast<int32_t>(s) << 16;
                }
                break;
            case 3:
                for (size_t i = 0; i < numSamples; i++) {
                    int32_t s = (src[i*3+2] << 24) | (src[i*3+1] << 16) | (src[i*3] << 8);
                    dst[i] = s;  // MSB-aligned
                }
                break;
            case 4:
                for (size_t i = 0; i < numSamples; i++) {
                    dst[i] = static_cast<int32_t>(
                        src[i*4] | (src[i*4+1] << 8) |
                        (src[i*4+2] << 16) | (src[i*4+3] << 24));
                }
                break;
        }
    }

    return numSamples;
}

uint32_t PcmDecoder::extendedToUint32(const uint8_t* bytes) {
    // IEEE 754 extended precision (80-bit) to uint32
    // Format: 1 sign + 15 exponent + 64 mantissa (with explicit integer bit)
    int exponent = ((bytes[0] & 0x7F) << 8) | bytes[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++) {
        mantissa = (mantissa << 8) | bytes[2 + i];
    }

    if (exponent == 0 && mantissa == 0) return 0;

    // Bias is 16383
    double f = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return static_cast<uint32_t>(f + 0.5);
}
