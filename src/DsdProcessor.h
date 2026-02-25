/**
 * @file DsdProcessor.h
 * @brief DSD format conversion utilities
 *
 * Not a decoder — DSD data is raw bitstream that doesn't need decoding.
 * This processor handles format conversions required between LMS's
 * interleaved DSD output and DirettaSync's planar input format.
 *
 * Conversions (conceptually from squeeze2diretta-wrapper, rewritten MIT):
 * - Native DSD: interleaved [L0R0L1R1...] -> planar [L0L1...R0R1...]
 * - DoP: extract DSD bits from S32_LE DoP frames -> planar native DSD
 */

#ifndef SLIM2DIRETTA_DSD_PROCESSOR_H
#define SLIM2DIRETTA_DSD_PROCESSOR_H

#include <cstdint>
#include <cstddef>
#include <vector>

class DsdProcessor {
public:
    /**
     * @brief De-interleave native DSD stereo to planar format
     *
     * Input:  interleaved DSD bytes [L0][R0][L1][R1]...
     * Output: planar DSD bytes [L0][L1][L2]...[R0][R1][R2]...
     *
     * @param src Interleaved DSD data
     * @param dst Planar output buffer (same size as src)
     * @param numBytes Total input bytes (must be even for stereo)
     * @param channels Number of channels (typically 2)
     */
    static void deinterleaveToPlaynar(const uint8_t* src, uint8_t* dst,
                                       size_t numBytes, uint32_t channels);

    /**
     * @brief De-interleave native DSD in U32_BE container to planar
     *
     * Input:  interleaved U32_BE frames [L3L2L1L0][R3R2R1R0]...
     *         (4 DSD bytes per channel per frame, big-endian byte order)
     * Output: planar DSD bytes with byte-swap to correct temporal order
     *         [L0L1L2L3...][R0R1R2R3...]
     *
     * @param src Interleaved U32_BE DSD frames
     * @param dst Planar output buffer
     * @param numFrames Number of U32 frames per channel
     * @param channels Number of channels
     */
    static void deinterleaveU32BE(const uint8_t* src, uint8_t* dst,
                                   size_t numFrames, uint32_t channels);

    /**
     * @brief Convert DoP (DSD over PCM) to native planar DSD
     *
     * Input:  S32_LE DoP frames, each 32-bit sample contains:
     *         byte[0]=pad, byte[1]=DSD_LSB, byte[2]=DSD_MSB, byte[3]=marker
     *         Interleaved stereo: [L_dop][R_dop][L_dop][R_dop]...
     * Output: planar native DSD [L_bytes...][R_bytes...]
     *
     * Each DoP sample yields 2 DSD bytes (16 DSD bits).
     *
     * @param src Interleaved DoP S32_LE samples
     * @param dst Planar DSD output
     * @param numFrames Number of stereo DoP frames
     * @param channels Number of channels
     */
    static void convertDopToNative(const uint8_t* src, uint8_t* dst,
                                    size_t numFrames, uint32_t channels);

    /**
     * @brief Calculate actual DSD bit rate from container rate
     * @param containerRate Sample rate from strm command
     * @param isDoP true if DoP format
     * @return DSD bit rate (e.g., 2822400 for DSD64)
     */
    static uint32_t calculateDsdRate(uint32_t containerRate, bool isDoP);

    /**
     * @brief Get human-readable DSD rate name
     * @param dsdBitRate DSD bit rate in Hz
     * @return "DSD64", "DSD128", etc.
     */
    static const char* rateName(uint32_t dsdBitRate);

    /**
     * @brief Calculate output buffer size for native de-interleave
     * @param inputBytes Input size in bytes
     * @return Output buffer size needed (same as input for byte-level de-interleave)
     */
    static size_t outputSizeNative(size_t inputBytes) { return inputBytes; }

    /**
     * @brief Calculate output buffer size for U32_BE de-interleave
     */
    static size_t outputSizeU32BE(size_t numFrames, uint32_t channels) {
        return numFrames * 4 * channels;
    }

    /**
     * @brief Calculate output buffer size for DoP conversion
     * @param numFrames Number of DoP stereo frames
     * @param channels Number of channels
     * @return Output size in bytes (2 DSD bytes per DoP sample per channel)
     */
    static size_t outputSizeDop(size_t numFrames, uint32_t channels) {
        return numFrames * 2 * channels;
    }
};

#endif // SLIM2DIRETTA_DSD_PROCESSOR_H
