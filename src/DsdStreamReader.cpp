/**
 * @file DsdStreamReader.cpp
 * @brief DSF/DFF container parser for DSD audio streams
 *
 * Parses DSF and DFF (DSDIFF) container headers, then outputs raw
 * planar DSD bytes [L0L1...][R0R1...] ready for DirettaSync.
 *
 * DSF: block-interleaved (already planar per block pair), LSB-first
 * DFF: byte-interleaved (needs de-interleaving), MSB-first
 */

#include "DsdStreamReader.h"
#include "DsdProcessor.h"
#include "globals.h"

#include <algorithm>
#include <cstring>

// ============================================================
// Read helpers
// ============================================================

uint32_t DsdStreamReader::readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t DsdStreamReader::readLE64(const uint8_t* p) {
    return static_cast<uint64_t>(readLE32(p))
         | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

uint32_t DsdStreamReader::readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

uint64_t DsdStreamReader::readBE64(const uint8_t* p) {
    return (static_cast<uint64_t>(readBE32(p)) << 32)
         | static_cast<uint64_t>(readBE32(p + 4));
}

// ============================================================
// Constructor / flush
// ============================================================

DsdStreamReader::DsdStreamReader() {
    m_headerBuf.reserve(256);
    m_dataBuf.reserve(65536);
}

void DsdStreamReader::flush() {
    m_state = State::DETECT;
    m_headerBuf.clear();
    m_dataBuf.clear();
    m_format = DsdFormat{};
    m_formatReady = false;
    m_rawDsdConfigured = false;
    m_dataRemaining = 0;
    m_totalBytesOutput = 0;
    m_eof = false;
    m_error = false;
    m_finished = false;
}

void DsdStreamReader::setRawDsdFormat(uint32_t dsdRate, uint32_t channels) {
    m_format.sampleRate = dsdRate;
    m_format.channels = channels;
    m_format.blockSizePerChannel = 0;
    m_format.totalDsdBytes = 0;
    m_format.container = DsdFormat::Container::RAW;
    m_format.isLSBFirst = false;  // Raw DSD: assume MSB-first (DFF convention)
    m_rawDsdConfigured = true;
}

// ============================================================
// feed / setEof
// ============================================================

size_t DsdStreamReader::feed(const uint8_t* data, size_t len) {
    if (m_state == State::DONE || m_state == State::ERROR) return len;

    if (m_state == State::DETECT || m_state == State::PARSE_DSF ||
        m_state == State::PARSE_DFF) {
        // Accumulate in header buffer until parsing succeeds
        m_headerBuf.insert(m_headerBuf.end(), data, data + len);

        if (m_state == State::DETECT) {
            detectContainer();
        }
        if (m_state == State::PARSE_DSF) {
            parseDsfHeader();
        } else if (m_state == State::PARSE_DFF) {
            parseDffHeader();
        }
    } else if (m_state == State::DATA) {
        // Append to data buffer, respecting dataRemaining
        size_t toAdd = len;
        if (m_dataRemaining > 0 && toAdd > m_dataRemaining) {
            toAdd = static_cast<size_t>(m_dataRemaining);
        }
        m_dataBuf.insert(m_dataBuf.end(), data, data + toAdd);
        if (m_dataRemaining > 0) {
            m_dataRemaining -= toAdd;
        }
    }

    return len;
}

void DsdStreamReader::setEof() {
    m_eof = true;
}

// ============================================================
// Container detection
// ============================================================

bool DsdStreamReader::detectContainer() {
    if (m_headerBuf.size() < 4) return false;

    const uint8_t* p = m_headerBuf.data();

    if (std::memcmp(p, "DSD ", 4) == 0) {
        m_state = State::PARSE_DSF;
        LOG_INFO("[DSD] Detected DSF container");
        return true;
    }

    if (std::memcmp(p, "FRM8", 4) == 0) {
        m_state = State::PARSE_DFF;
        LOG_INFO("[DSD] Detected DFF (DSDIFF) container");
        return true;
    }

    // No known container — check for raw DSD mode
    if (m_rawDsdConfigured) {
        m_formatReady = true;
        m_dataRemaining = 0;  // Unlimited
        // Move header bytes to data buffer (they're DSD data)
        m_dataBuf.insert(m_dataBuf.end(), m_headerBuf.begin(), m_headerBuf.end());
        m_headerBuf.clear();
        m_state = State::DATA;
        LOG_INFO("[DSD] Raw DSD: " << m_format.sampleRate << " Hz, "
                 << m_format.channels << " ch");
        return true;
    }

    uint32_t magic = readBE32(p);
    LOG_ERROR("[DSD] Unknown container magic: 0x" << std::hex << magic << std::dec);
    m_state = State::ERROR;
    m_error = true;
    return false;
}

// ============================================================
// DSF header parsing (little-endian)
// ============================================================

bool DsdStreamReader::parseDsfHeader() {
    // DSF structure:
    //   "DSD " chunk (28 bytes): magic(4) + chunkSize(8) + totalFileSize(8) + metadataOffset(8)
    //   "fmt " chunk (52 bytes): magic(4) + chunkSize(8) + formatVersion(4) + formatID(4) +
    //                            channelType(4) + channelCount(4) + sampleRate(4) +
    //                            bitsPerSample(4) + sampleCount(8) + blockSizePerChannel(4) + reserved(4)
    //   "data" chunk header (12 bytes): magic(4) + chunkSize(8)

    constexpr size_t DSF_MIN_HEADER = 28 + 52 + 12;  // 92 bytes minimum
    if (m_headerBuf.size() < DSF_MIN_HEADER) return false;

    const uint8_t* p = m_headerBuf.data();

    // Validate DSD chunk
    if (std::memcmp(p, "DSD ", 4) != 0) {
        LOG_ERROR("[DSD] DSF: invalid DSD chunk magic");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    // fmt chunk starts at offset 28
    if (std::memcmp(p + 28, "fmt ", 4) != 0) {
        LOG_ERROR("[DSD] DSF: missing fmt chunk at offset 28");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    uint64_t fmtChunkSize = readLE64(p + 32);
    uint32_t formatVersion = readLE32(p + 40);
    uint32_t formatID = readLE32(p + 44);
    uint32_t channelType = readLE32(p + 48);
    uint32_t channelCount = readLE32(p + 52);
    uint32_t sampleRate = readLE32(p + 56);
    uint32_t bitsPerSample = readLE32(p + 60);
    uint64_t sampleCount = readLE64(p + 64);
    uint32_t blockSize = readLE32(p + 72);

    (void)formatVersion;
    (void)channelType;
    (void)fmtChunkSize;

    if (formatID != 0) {
        LOG_ERROR("[DSD] DSF: unsupported format ID " << formatID << " (expected 0 = DSD Raw)");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    if (bitsPerSample != 1) {
        LOG_WARN("[DSD] DSF: bitsPerSample=" << bitsPerSample << " (expected 1)");
    }

    if (channelCount == 0 || channelCount > 8) {
        LOG_ERROR("[DSD] DSF: invalid channel count " << channelCount);
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    if (blockSize == 0) {
        LOG_ERROR("[DSD] DSF: invalid block size 0");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    // data chunk starts after DSD (28) + fmt (fmtChunkSize or 52)
    size_t dataChunkOffset = 28 + static_cast<size_t>(fmtChunkSize);
    if (m_headerBuf.size() < dataChunkOffset + 12) return false;

    if (std::memcmp(p + dataChunkOffset, "data", 4) != 0) {
        LOG_ERROR("[DSD] DSF: missing data chunk at offset " << dataChunkOffset);
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    uint64_t dataChunkSize = readLE64(p + dataChunkOffset + 4);
    uint64_t dataBytes = dataChunkSize - 12;  // Subtract chunk header

    // Set format
    m_format.sampleRate = sampleRate;
    m_format.channels = channelCount;
    m_format.blockSizePerChannel = blockSize;
    m_format.totalDsdBytes = dataBytes;
    m_format.container = DsdFormat::Container::DSF;
    m_format.isLSBFirst = true;

    m_dataRemaining = dataBytes;
    m_formatReady = true;

    LOG_INFO("[DSD] DSF: " << DsdProcessor::rateName(sampleRate) << " ("
             << sampleRate << " Hz), " << channelCount << " ch, "
             << "block=" << blockSize << ", data=" << dataBytes << " bytes"
             << ", samples/ch=" << sampleCount);

    // Move remaining header bytes to data buffer
    size_t dataStart = dataChunkOffset + 12;
    if (m_headerBuf.size() > dataStart) {
        size_t excess = m_headerBuf.size() - dataStart;
        size_t toMove = excess;
        if (m_dataRemaining > 0 && toMove > m_dataRemaining) {
            toMove = static_cast<size_t>(m_dataRemaining);
        }
        m_dataBuf.insert(m_dataBuf.end(),
                         m_headerBuf.begin() + dataStart,
                         m_headerBuf.begin() + dataStart + toMove);
        if (m_dataRemaining > 0) {
            m_dataRemaining -= toMove;
        }
    }
    m_headerBuf.clear();
    m_state = State::DATA;
    return true;
}

// ============================================================
// DFF (DSDIFF) header parsing (big-endian)
// ============================================================

bool DsdStreamReader::parseDffHeader() {
    // DFF structure:
    //   "FRM8" (4) + size (8) + "DSD " (4) = 16 bytes for outer container
    //   Then sub-chunks, each with: ID (4) + size (8) + data
    //   Key chunks: "FVER", "PROP" (contains "FS  ", "CHNL", "CMPR"), "DSD " (data)

    if (m_headerBuf.size() < 16) return false;

    const uint8_t* p = m_headerBuf.data();
    size_t bufSize = m_headerBuf.size();

    // Validate FRM8 + DSD form type
    if (std::memcmp(p, "FRM8", 4) != 0 || std::memcmp(p + 12, "DSD ", 4) != 0) {
        LOG_ERROR("[DSD] DFF: invalid FRM8/DSD header");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    bool foundFS = false;
    bool foundCHNL = false;
    bool foundData = false;
    size_t dataStart = 0;
    uint64_t dataSize = 0;

    // Scan sub-chunks starting at offset 16
    size_t pos = 16;
    while (pos + 12 <= bufSize) {
        char chunkId[5] = {0};
        std::memcpy(chunkId, p + pos, 4);
        uint64_t chunkSize = readBE64(p + pos + 4);

        if (std::memcmp(chunkId, "FVER", 4) == 0) {
            // Format version — skip
            pos += 12 + static_cast<size_t>(chunkSize);
            continue;
        }

        if (std::memcmp(chunkId, "PROP", 4) == 0) {
            // Property chunk with "SND " form type
            if (pos + 16 > bufSize) return false;  // Need more data
            if (std::memcmp(p + pos + 12, "SND ", 4) != 0) {
                pos += 12 + static_cast<size_t>(chunkSize);
                continue;
            }

            // Scan sub-sub-chunks inside PROP
            size_t propEnd = pos + 12 + static_cast<size_t>(chunkSize);
            size_t subPos = pos + 16;  // After PROP header + "SND "

            while (subPos + 12 <= bufSize && subPos + 12 <= propEnd) {
                char subId[5] = {0};
                std::memcpy(subId, p + subPos, 4);
                uint64_t subSize = readBE64(p + subPos + 4);

                if (std::memcmp(subId, "FS  ", 4) == 0) {
                    if (subPos + 12 + 4 > bufSize) return false;
                    sampleRate = readBE32(p + subPos + 12);
                    foundFS = true;
                } else if (std::memcmp(subId, "CHNL", 4) == 0) {
                    if (subPos + 12 + 2 > bufSize) return false;
                    channels = (static_cast<uint32_t>(p[subPos + 12]) << 8)
                             | static_cast<uint32_t>(p[subPos + 13]);
                    foundCHNL = true;
                } else if (std::memcmp(subId, "CMPR", 4) == 0) {
                    if (subPos + 12 + 4 > bufSize) return false;
                    if (std::memcmp(p + subPos + 12, "DSD ", 4) != 0) {
                        LOG_ERROR("[DSD] DFF: compressed DSD not supported");
                        m_state = State::ERROR;
                        m_error = true;
                        return false;
                    }
                }

                subPos += 12 + static_cast<size_t>(subSize);
                // DFF chunks are word-aligned (2-byte)
                if (subPos & 1) subPos++;
            }

            pos = propEnd;
            // Word-align
            if (pos & 1) pos++;
            continue;
        }

        if (std::memcmp(chunkId, "DSD ", 4) == 0) {
            // Audio data chunk
            dataSize = chunkSize;
            dataStart = pos + 12;
            foundData = true;
            break;
        }

        // Skip unknown chunks
        pos += 12 + static_cast<size_t>(chunkSize);
        if (pos & 1) pos++;  // Word-align
    }

    if (!foundData) {
        // Need more header data
        return false;
    }

    if (!foundFS || sampleRate == 0) {
        LOG_ERROR("[DSD] DFF: missing FS (sample rate) chunk");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    if (!foundCHNL || channels == 0) {
        LOG_ERROR("[DSD] DFF: missing CHNL (channels) chunk");
        m_state = State::ERROR;
        m_error = true;
        return false;
    }

    // Set format
    m_format.sampleRate = sampleRate;
    m_format.channels = channels;
    m_format.blockSizePerChannel = 0;  // DFF is byte-interleaved, no blocks
    m_format.totalDsdBytes = dataSize;
    m_format.container = DsdFormat::Container::DFF;
    m_format.isLSBFirst = false;  // DFF is MSB-first

    m_dataRemaining = dataSize;
    m_formatReady = true;

    LOG_INFO("[DSD] DFF: " << DsdProcessor::rateName(sampleRate) << " ("
             << sampleRate << " Hz), " << channels << " ch, "
             << "data=" << dataSize << " bytes");

    // Move excess bytes to data buffer
    if (m_headerBuf.size() > dataStart) {
        size_t excess = m_headerBuf.size() - dataStart;
        size_t toMove = excess;
        if (m_dataRemaining > 0 && toMove > m_dataRemaining) {
            toMove = static_cast<size_t>(m_dataRemaining);
        }
        m_dataBuf.insert(m_dataBuf.end(),
                         m_headerBuf.begin() + dataStart,
                         m_headerBuf.begin() + dataStart + toMove);
        if (m_dataRemaining > 0) {
            m_dataRemaining -= toMove;
        }
    }
    m_headerBuf.clear();
    m_state = State::DATA;
    return true;
}

// ============================================================
// Data processing
// ============================================================

size_t DsdStreamReader::processDsfBlocks(uint8_t* out, size_t maxBytes) {
    // DSF block structure: [blockSize bytes L][blockSize bytes R][blockSize L][blockSize R]...
    // One block group = blockSizePerChannel * channels bytes
    // Each block group is already planar: [L block][R block]

    uint32_t blockGroup = m_format.blockSizePerChannel * m_format.channels;
    if (blockGroup == 0) return 0;

    size_t avail = m_dataBuf.size();
    size_t maxGroups = maxBytes / blockGroup;
    size_t availGroups = avail / blockGroup;
    size_t groups = std::min(maxGroups, availGroups);

    if (groups == 0) {
        // At EOF, handle partial last block group
        if (m_eof && avail > 0 && m_dataRemaining == 0) {
            // Partial block at end — align to channels
            size_t usable = (avail / m_format.channels) * m_format.channels;
            if (usable > maxBytes) usable = (maxBytes / m_format.channels) * m_format.channels;
            if (usable == 0) return 0;
            std::memcpy(out, m_dataBuf.data(), usable);
            m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + usable);
            m_totalBytesOutput += usable;
            return usable;
        }
        return 0;
    }

    size_t bytes = groups * blockGroup;
    // DSF blocks are already planar — direct copy
    std::memcpy(out, m_dataBuf.data(), bytes);
    m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + bytes);
    m_totalBytesOutput += bytes;
    return bytes;
}

size_t DsdStreamReader::processDffData(uint8_t* out, size_t maxBytes) {
    // DFF data is byte-interleaved: [L0][R0][L1][R1]...
    // Need to de-interleave to planar: [L0L1...][R0R1...]

    size_t avail = m_dataBuf.size();
    if (avail == 0) return 0;

    uint32_t ch = m_format.channels;
    if (ch == 0) return 0;

    // Ensure byte count is a multiple of channels
    size_t usable = std::min(avail, maxBytes);
    usable = (usable / ch) * ch;
    if (usable == 0) return 0;

    DsdProcessor::deinterleaveToPlaynar(m_dataBuf.data(), out, usable, ch);
    m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + usable);
    m_totalBytesOutput += usable;
    return usable;
}

size_t DsdStreamReader::processRawData(uint8_t* out, size_t maxBytes) {
    // Raw DSD: assume byte-interleaved like DFF
    return processDffData(out, maxBytes);
}

// ============================================================
// readPlanar — main output method
// ============================================================

size_t DsdStreamReader::readPlanar(uint8_t* out, size_t maxBytes) {
    if (m_state == State::DONE || m_state == State::ERROR) return 0;
    if (m_state != State::DATA) return 0;
    if (!m_formatReady) return 0;

    size_t result = 0;

    switch (m_format.container) {
        case DsdFormat::Container::DSF:
            result = processDsfBlocks(out, maxBytes);
            break;
        case DsdFormat::Container::DFF:
            result = processDffData(out, maxBytes);
            break;
        case DsdFormat::Container::RAW:
            result = processRawData(out, maxBytes);
            break;
    }

    // Check if we're done
    if (result == 0 && m_dataBuf.empty()) {
        if (m_eof) {
            m_finished = true;
            m_state = State::DONE;
        }
    }

    return result;
}
