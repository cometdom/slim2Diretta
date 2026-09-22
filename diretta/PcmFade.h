// 10 ms smoothstep PCM fade ramps around cuts in the middle of the music
// (Stop, Pause, track skip, rebuffering). Ported from DirettaRendererUPnP
// (PR #98, herisson-88) — same math, same integer gain, unchanged.

#ifndef PCM_FADE_H
#define PCM_FADE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

// Short ramps around the places where PCM playback is cut or restarted in the
// middle of the music (Stop, Pause, seek, resume, rebuffering). Going from a
// music buffer straight to a zero buffer, or back, is a step in the waveform,
// heard as a click whose level depends on where the cut lands.
// Integer only, no allocation, no I/O: safe in the Diretta callback thread.
// Little-endian host and arithmetic right shift of negative values assumed,
// like the rest of the ring path (GCC/Clang on x86 and ARM).
namespace PcmFade {

// Length of both ramps
constexpr int FADE_MS = 10;

// Longest ramp gainQ16() computes exactly in 64 bits (3.2 MHz at 10 ms)
constexpr uint32_t MAX_FADE_FRAMES = 32768;

inline uint32_t fadeFramesForRate(int sampleRate) {
    if (sampleRate <= 0) return 0;
    uint32_t frames = static_cast<uint32_t>(sampleRate) * FADE_MS / 1000;
    return frames < MAX_FADE_FRAMES ? frames : MAX_FADE_FRAMES;
}

// Q16 gain for `remaining` frames left out of `total`: smoothstep
// t*t*(3-2t) with t = remaining/total, so the ramp has no corner at either end
// (1.0 at remaining == total, 0 at 0). Computed as the exact rational
// r*r*(3N-2r) / N^3, floored once: monotonic for every ramp length.
inline uint32_t gainQ16(uint32_t remaining, uint32_t total) {
    if (total == 0 || remaining == 0) return 0;
    if (remaining >= total) return 65536;
    if (total > MAX_FADE_FRAMES) return 0;  // would overflow; fadeFramesForRate() never asks for it
    uint64_t r = remaining, n = total;
    return static_cast<uint32_t>(((r * r * (3 * n - 2 * r)) << 16) / (n * n * n));
}

// DoP carried as plain PCM (pre-encoded by the media server) cannot be scaled:
// the 0x05/0xFA marker sits in the top byte of every sample and alternates
// frame by frame. Only meaningful for 3- and 4-byte samples.
inline bool looksLikeDoP(const uint8_t* buf, size_t frames, int channels, int bytesPerSample) {
    if (bytesPerSample < 3 || channels <= 0 || frames < 2) return false;
    size_t frameBytes = static_cast<size_t>(channels) * bytesPerSample;
    uint8_t m0 = buf[bytesPerSample - 1];
    uint8_t m1 = buf[frameBytes + bytesPerSample - 1];
    return (m0 == 0x05 && m1 == 0xFA) || (m0 == 0xFA && m1 == 0x05);
}

// One interleaved little-endian signed frame scaled in place by a Q16 gain,
// rounded to nearest.
inline void scaleFrame(uint8_t* p, int channels, int bytesPerSample, int64_t g) {
    for (int c = 0; c < channels; c++, p += bytesPerSample) {
        switch (bytesPerSample) {
            case 2: {
                int16_t s;
                std::memcpy(&s, p, 2);
                s = static_cast<int16_t>((s * g + 0x8000) >> 16);
                std::memcpy(p, &s, 2);
                break;
            }
            case 3: {
                int32_t s = static_cast<int32_t>(
                    (static_cast<uint32_t>(p[0]) << 8) |
                    (static_cast<uint32_t>(p[1]) << 16) |
                    (static_cast<uint32_t>(p[2]) << 24)) >> 8;
                s = static_cast<int32_t>((s * g + 0x8000) >> 16);
                p[0] = static_cast<uint8_t>(s);
                p[1] = static_cast<uint8_t>(s >> 8);
                p[2] = static_cast<uint8_t>(s >> 16);
                break;
            }
            case 4: {
                int32_t s;
                std::memcpy(&s, p, 4);
                s = static_cast<int32_t>((s * g + 0x8000) >> 16);
                std::memcpy(p, &s, 4);
                break;
            }
            default:
                break;
        }
    }
}

// Fade-out: scales `frames` frames in place and counts them off `remaining`.
// Frames past the end of the ramp are zeroed.
inline void applyFadeOut(uint8_t* buf, size_t frames, int channels, int bytesPerSample,
                         uint32_t& remaining, uint32_t total) {
    if (channels <= 0) return;
    size_t frameBytes = static_cast<size_t>(channels) * bytesPerSample;
    for (size_t f = 0; f < frames; f++, buf += frameBytes) {
        if (remaining > 0) remaining--;
        scaleFrame(buf, channels, bytesPerSample, gainQ16(remaining, total));
    }
}

// Fade-in: the mirror ramp, from zero on the first frame up to the music
// level. Frames past the end of the ramp are left untouched.
inline void applyFadeIn(uint8_t* buf, size_t frames, int channels, int bytesPerSample,
                        uint32_t& remaining, uint32_t total) {
    if (channels <= 0) return;
    size_t frameBytes = static_cast<size_t>(channels) * bytesPerSample;
    for (size_t f = 0; f < frames && remaining > 0; f++, buf += frameBytes) {
        scaleFrame(buf, channels, bytesPerSample, gainQ16(total - remaining, total));
        remaining--;
    }
}

} // namespace PcmFade

#endif // PCM_FADE_H
