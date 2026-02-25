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
    unsigned int cycleTime = 2620;      // microseconds between packets
    bool cycleTimeAuto = true;          // compute from MTU + format
    unsigned int mtu = 0;               // 0 = auto-detect

    // Audio
    int maxSampleRate = 768000;
    bool dsdEnabled = true;

    // Logging
    bool verbose = false;
    bool quiet = false;

    // Actions
    bool listTargets = false;
    bool showVersion = false;
};

#endif // SLIM2DIRETTA_CONFIG_H
