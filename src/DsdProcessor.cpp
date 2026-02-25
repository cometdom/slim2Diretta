/**
 * @file DsdProcessor.cpp
 * @brief DSD format conversion utilities
 *
 * All conversions produce planar DSD data: [L0L1L2...][R0R1R2...]
 * which is what DirettaSync expects for DSD playback.
 */

#include "DsdProcessor.h"
#include <cstring>

void DsdProcessor::deinterleaveToPlaynar(const uint8_t* src, uint8_t* dst,
                                          size_t numBytes, uint32_t channels) {
    if (channels < 2) {
        // Mono: just copy
        std::memcpy(dst, src, numBytes);
        return;
    }

    // Stereo: separate [L0R0L1R1...] into [L0L1...][R0R1...]
    size_t bytesPerChannel = numBytes / channels;

    for (size_t i = 0; i < bytesPerChannel; i++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            dst[ch * bytesPerChannel + i] = src[i * channels + ch];
        }
    }
}

void DsdProcessor::deinterleaveU32BE(const uint8_t* src, uint8_t* dst,
                                      size_t numFrames, uint32_t channels) {
    // Input: interleaved U32_BE frames
    //   Each frame per channel = 4 bytes in big-endian (MSB first temporal order)
    //   Interleaved: [L3L2L1L0][R3R2R1R0]...
    //
    // Output: planar with byte-swap to restore correct temporal order
    //   [L0L1L2L3...][R0R1R2R3...]
    //
    // The byte-swap is needed because the U32_BE packing has the first
    // temporal DSD byte at the highest address within the uint32.

    size_t bytesPerChannel = numFrames * 4;
    size_t bytesPerFrame = 4 * channels;

    for (size_t frame = 0; frame < numFrames; frame++) {
        size_t srcOffset = frame * bytesPerFrame;

        for (uint32_t ch = 0; ch < channels; ch++) {
            size_t chSrcOffset = srcOffset + ch * 4;
            size_t dstOffset = ch * bytesPerChannel + frame * 4;

            // Byte-swap: reverse 4 bytes to correct temporal order
            dst[dstOffset + 0] = src[chSrcOffset + 3];
            dst[dstOffset + 1] = src[chSrcOffset + 2];
            dst[dstOffset + 2] = src[chSrcOffset + 1];
            dst[dstOffset + 3] = src[chSrcOffset + 0];
        }
    }
}

void DsdProcessor::convertDopToNative(const uint8_t* src, uint8_t* dst,
                                       size_t numFrames, uint32_t channels) {
    // Input: S32_LE DoP frames, interleaved
    //   Each 32-bit sample (little-endian in memory):
    //     byte[0] = padding (0x00)
    //     byte[1] = DSD_LSB
    //     byte[2] = DSD_MSB
    //     byte[3] = marker (0x05 or 0xFA alternating)
    //
    // Output: planar native DSD, 2 bytes per DoP sample, MSB first (DFF order)
    //   [L_MSB0 L_LSB0 L_MSB1 L_LSB1 ...][R_MSB0 R_LSB0 ...]

    size_t dsdBytesPerSample = 2;  // Each DoP sample yields 2 DSD bytes
    size_t bytesPerChannel = numFrames * dsdBytesPerSample;
    size_t srcBytesPerFrame = 4 * channels;  // S32_LE per channel

    for (size_t frame = 0; frame < numFrames; frame++) {
        size_t srcOffset = frame * srcBytesPerFrame;

        for (uint32_t ch = 0; ch < channels; ch++) {
            size_t chSrcOffset = srcOffset + ch * 4;
            size_t dstOffset = ch * bytesPerChannel + frame * dsdBytesPerSample;

            // Extract DSD bytes (MSB first for DFF format)
            dst[dstOffset + 0] = src[chSrcOffset + 2];  // DSD MSB
            dst[dstOffset + 1] = src[chSrcOffset + 1];  // DSD LSB
        }
    }
}

uint32_t DsdProcessor::calculateDsdRate(uint32_t containerRate, bool isDoP) {
    if (isDoP) {
        // DoP: each PCM sample carries 16 DSD bits
        return containerRate * 16;
    }
    // Native DSD in U32 container: each frame carries 32 DSD bits
    return containerRate * 32;
}

const char* DsdProcessor::rateName(uint32_t dsdBitRate) {
    // DSD64 = 2.8224 MHz, each doubling = DSD128, DSD256, etc.
    if (dsdBitRate <= 2900000)   return "DSD64";
    if (dsdBitRate <= 5700000)   return "DSD128";
    if (dsdBitRate <= 11400000)  return "DSD256";
    if (dsdBitRate <= 22800000)  return "DSD512";
    if (dsdBitRate <= 45600000)  return "DSD1024";
    return "DSD???";
}
