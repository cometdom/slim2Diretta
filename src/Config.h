/**
 * @file Config.h
 * @brief Configuration for slim2diretta
 */

#ifndef SLIM2DIRETTA_CONFIG_H
#define SLIM2DIRETTA_CONFIG_H

#include <string>
#include <cstdint>

struct Config {
    // LMS connection
    std::string lmsServer;              // empty = autodiscovery (Phase 6)
    uint16_t lmsPort = 3483;            // Slimproto TCP port
    std::string playerName = "slim2diretta";
    std::string macAddress;             // empty = auto-generate

    // Diretta
    int direttaTarget = -1;             // -1 = not set (required)
    int threadMode = 1;                 // SDK thread priority mode
    unsigned int cycleTime = 10000;     // microseconds between packets
    bool cycleTimeAuto = true;          // compute from MTU + format
    unsigned int mtu = 0;               // 0 = auto-detect
    std::string transferMode;           // "auto","varmax","varauto","fixauto","random" (empty=auto)
    unsigned int infoCycle = 100000;    // Info packet cycle µs (default 100ms)
    unsigned int cycleMinTime = 0;      // Min cycle for RANDOM mode (0 = unused)
    unsigned int targetProfileLimitTime = 0;   // 0=SelfProfile (stable), >0=TargetProfile(µs)

    // CPU affinity (empty = no pinning). Accepts comma-separated cores: "6" or "6,7,8"
    std::string cpuAudio;               // Core(s) for SDK worker + Diretta hot path
    std::string cpuDecode;              // Core(s) for the audio/decode thread (HTTP→decode→push)
    std::string cpuOther;               // Core(s) for main + slimproto threads

    // Buffer configuration (0 = use built-in defaults from DirettaSync)
    // Note: slim2Diretta receives audio from LMS locally, no remote-specific variant.
    float pcmBufferSeconds = 0.0f;         // PCM buffer size in seconds
    float dsdBufferSeconds = 0.0f;         // DSD buffer size in seconds
    unsigned int pcmPrefillMs = 0;         // PCM prefill duration in ms
    unsigned int dsdPrefillMs = 0;         // DSD prefill duration in ms

    // Audio
    int maxSampleRate = 1536000;
    bool dsdEnabled = true;
    std::string decoderBackend = "native";  // "native" or "ffmpeg"

    // Logging
    bool verbose = false;
    bool quiet = false;

    // Actions
    bool listTargets = false;
    bool showVersion = false;
};

#endif // SLIM2DIRETTA_CONFIG_H
