// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Dominique COMET (cometdom)
// This file is part of slim2diretta. See LICENSE for details.

/**
 * @file OggFlacDecoder.h
 * @brief Ogg FLAC support in front of the FLAC decoders
 *
 * LMS sends Ogg FLAC (internet radio) with the same 'f' format code as FLAC,
 * passing it through untouched only to players that advertise "ogf". This
 * wrapper sniffs the first bytes: plain FLAC goes straight to the wrapped
 * decoder, while an Ogg stream is unwrapped here (Ogg pages → FLAC packets,
 * per the Xiph Ogg FLAC mapping) into a native FLAC stream.
 *
 * Radio streams chain one Ogg logical stream per song. A new link with the
 * same format keeps feeding the same decoder (its header packets are dropped),
 * so this works whatever the libFLAC version (chained Ogg decoding only exists
 * in libFLAC 1.5+). A link with a different format ends the stream for now.
 */

#ifndef SLIM2DIRETTA_OGGFLACDECODER_H
#define SLIM2DIRETTA_OGGFLACDECODER_H

#include "Decoder.h"

#include <vector>

class OggFlacDecoder : public Decoder {
public:
    explicit OggFlacDecoder(std::unique_ptr<Decoder> inner);

    size_t feed(const uint8_t* data, size_t len) override;
    void setEof() override;
    size_t readDecoded(int32_t* out, size_t maxFrames) override;
    bool isFormatReady() const override { return m_inner->isFormatReady(); }
    DecodedFormat getFormat() const override { return m_inner->getFormat(); }
    bool isFinished() const override { return m_inner->isFinished(); }
    // An unsupported format change ends the stream while HTTP still flows;
    // report it once drained so the playback loop stops instead of idling.
    bool hasError() const override {
        return m_inner->hasError() || (m_stopped && m_inner->isFinished());
    }
    uint64_t getDecodedSamples() const override { return m_inner->getDecodedSamples(); }
    void flush() override;

private:
    enum class Mode { Sniffing, Plain, Ogg };

    void parsePages();
    void handlePacket(uint32_t serial);
    void startLink(uint32_t serial);
    void endStream();

    std::unique_ptr<Decoder> m_inner;
    Mode m_mode = Mode::Sniffing;
    std::vector<uint8_t> m_in;          // Unparsed Ogg bytes
    size_t m_inPos = 0;
    std::vector<uint8_t> m_packet;      // Packet being assembled across pages
    std::vector<uint8_t> m_out;         // Native FLAC bytes for the wrapped decoder

    bool m_haveLink = false;            // A FLAC logical stream has started
    bool m_linkHeaderPending = false;   // Next packet is the link's first packet
    bool m_stopped = false;             // Stream ended (unsupported format change)
    uint32_t m_serial = 0;
    uint32_t m_prevSerial = 0;
    uint32_t m_links = 0;
    uint8_t m_streamInfo[34] = {};      // STREAMINFO body of the first link
};

#endif // SLIM2DIRETTA_OGGFLACDECODER_H
