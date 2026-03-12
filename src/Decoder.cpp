/**
 * @file Decoder.cpp
 * @brief Decoder factory implementation
 */

#include "Decoder.h"
#include "FlacDecoder.h"
#include "PcmDecoder.h"
#include "SlimprotoMessages.h"
#include "LogLevel.h"

#ifdef ENABLE_MP3
#include "Mp3Decoder.h"
#endif
#ifdef ENABLE_OGG
#include "OggDecoder.h"
#endif
#ifdef ENABLE_AAC
#include "AacDecoder.h"
#endif
#ifdef ENABLE_FFMPEG
#include "FfmpegDecoder.h"
#endif

std::unique_ptr<Decoder> Decoder::create(char formatCode,
                                          const std::string& backend) {
#ifdef ENABLE_FFMPEG
    // FFmpeg backend handles all formats except DSD (raw bitstream)
    if (backend == "ffmpeg" && formatCode != FORMAT_DSD) {
        LOG_DEBUG("[Decoder] Using FFmpeg backend for format '" << formatCode << "'");
        return std::make_unique<FfmpegDecoder>(formatCode);
    }
#else
    if (backend == "ffmpeg") {
        LOG_WARN("[Decoder] FFmpeg backend requested but not compiled in — using native");
    }
#endif

    switch (formatCode) {
        case FORMAT_FLAC:
            return std::make_unique<FlacDecoder>();
        case FORMAT_PCM:
            return std::make_unique<PcmDecoder>();
#ifdef ENABLE_MP3
        case FORMAT_MP3:
            return std::make_unique<Mp3Decoder>();
#endif
#ifdef ENABLE_OGG
        case FORMAT_OGG:
            return std::make_unique<OggDecoder>();
#endif
#ifdef ENABLE_AAC
        case FORMAT_AAC:
            return std::make_unique<AacDecoder>();
#endif
        // DSD (FORMAT_DSD) is not decoded — handled by DsdProcessor
        default:
            return nullptr;
    }
}
