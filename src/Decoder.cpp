/**
 * @file Decoder.cpp
 * @brief Decoder factory implementation
 */

#include "Decoder.h"
#include "FlacDecoder.h"
#include "PcmDecoder.h"
#include "SlimprotoMessages.h"

std::unique_ptr<Decoder> Decoder::create(char formatCode) {
    switch (formatCode) {
        case FORMAT_FLAC:
            return std::make_unique<FlacDecoder>();
        case FORMAT_PCM:
            return std::make_unique<PcmDecoder>();
        // DSD (FORMAT_DSD) is not decoded — handled by DsdProcessor
        // AAC, MP3, OGG, WMA, ALAC: not supported yet
        default:
            return nullptr;
    }
}
