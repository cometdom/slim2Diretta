/**
 * @file SlimprotoMessages.h
 * @brief Binary message definitions for the Slimproto protocol
 *
 * Protocol reference: wiki.lyrion.org, Rust slimproto crate (MIT)
 * All multi-byte fields are network byte order (big-endian).
 *
 * Framing:
 *   Client -> Server: [4 opcode][4 length BE][payload]
 *   Server -> Client: [2 length BE][4 opcode][payload]
 */

#ifndef SLIM2DIRETTA_SLIMPROTO_MESSAGES_H
#define SLIM2DIRETTA_SLIMPROTO_MESSAGES_H

#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

// ============================================
// Protocol Constants
// ============================================

constexpr uint16_t SLIMPROTO_PORT = 3483;
constexpr uint16_t SLIMPROTO_HTTP_PORT = 9000;

// Device IDs for HELO
constexpr uint8_t DEVICE_ID_SQUEEZEBOX2 = 4;
constexpr uint8_t DEVICE_ID_TRANSPORTER = 5;
constexpr uint8_t DEVICE_ID_SQUEEZESLAVE = 8;

// strm sub-commands
constexpr char STRM_START   = 's';
constexpr char STRM_STOP    = 'q';
constexpr char STRM_PAUSE   = 'p';
constexpr char STRM_UNPAUSE = 'u';
constexpr char STRM_FLUSH   = 'f';
constexpr char STRM_STATUS  = 't';
constexpr char STRM_SKIP    = 'a';

// strm format codes
constexpr char FORMAT_PCM  = 'p';
constexpr char FORMAT_MP3  = 'm';
constexpr char FORMAT_FLAC = 'f';
constexpr char FORMAT_WMA  = 'w';
constexpr char FORMAT_OGG  = 'o';
constexpr char FORMAT_AAC  = 'a';
constexpr char FORMAT_ALAC = 'l';
constexpr char FORMAT_DSD  = 'd';

// strm autostart values
constexpr char AUTOSTART_NONE    = '0';
constexpr char AUTOSTART_AUTO    = '1';
constexpr char AUTOSTART_DIRECT  = '2';
constexpr char AUTOSTART_DIRECT_AUTO = '3';

// PCM sample size codes
constexpr char PCM_SIZE_8  = '0';
constexpr char PCM_SIZE_16 = '1';
constexpr char PCM_SIZE_20 = '2';
constexpr char PCM_SIZE_24 = '3';
constexpr char PCM_SIZE_32 = '4';
constexpr char PCM_SIZE_SELF = '?';

// PCM sample rate codes
constexpr char PCM_RATE_11K  = '0';
constexpr char PCM_RATE_22K  = '1';
constexpr char PCM_RATE_32K  = '2';
constexpr char PCM_RATE_44K  = '3';
constexpr char PCM_RATE_48K  = '4';
constexpr char PCM_RATE_8K   = '5';
constexpr char PCM_RATE_12K  = '6';
constexpr char PCM_RATE_16K  = '7';
constexpr char PCM_RATE_24K  = '8';
constexpr char PCM_RATE_96K  = '9';
constexpr char PCM_RATE_SELF = '?';

// PCM channel codes
constexpr char PCM_CHANNELS_MONO   = '1';
constexpr char PCM_CHANNELS_STEREO = '2';
constexpr char PCM_CHANNELS_SELF   = '?';

// PCM endianness codes
constexpr char PCM_ENDIAN_BIG    = '0';
constexpr char PCM_ENDIAN_LITTLE = '1';
constexpr char PCM_ENDIAN_SELF   = '?';

// STAT event codes (4 bytes each)
namespace StatEvent {
    constexpr char STMa[] = "STMa";  // Autostart
    constexpr char STMc[] = "STMc";  // Connected
    constexpr char STMd[] = "STMd";  // Decoder ready
    constexpr char STMe[] = "STMe";  // Connection established
    constexpr char STMf[] = "STMf";  // Flushed
    constexpr char STMh[] = "STMh";  // HTTP headers received
    constexpr char STMl[] = "STMl";  // Buffer threshold reached
    constexpr char STMn[] = "STMn";  // Not connected / decoder error
    constexpr char STMo[] = "STMo";  // Output underrun
    constexpr char STMp[] = "STMp";  // Pause confirmed
    constexpr char STMr[] = "STMr";  // Resume confirmed
    constexpr char STMs[] = "STMs";  // Track started
    constexpr char STMt[] = "STMt";  // Timer heartbeat response
    constexpr char STMu[] = "STMu";  // Underrun / end of track
}

// ============================================
// Server -> Client: strm command payload
// ============================================

struct StrmCommand {
    char command;           // 's', 'q', 'p', 'u', 'f', 't', 'a'
    char autostart;         // '0'-'3'
    char format;            // 'p', 'f', 'd', etc.
    char pcmSampleSize;     // '0'-'4', '?'
    char pcmSampleRate;     // '0'-'9', '?'
    char pcmChannels;       // '1', '2', '?'
    char pcmEndian;         // '0', '1', '?'
    uint8_t threshold;      // KB before autostart
    char spdifEnable;       // '0', '1', '2'
    uint8_t transPeriod;    // fade seconds
    char transType;         // '0'-'4'
    uint8_t flags;          // 0x80=loop, 0x40=no-restart
    uint8_t outputThreshold;// tenths of second
    uint8_t reserved;
    uint32_t replayGainOrInterval; // big-endian, 16.16 fixed-point (or interval for p/u/t)
    uint16_t serverPort;    // big-endian (default 9000)
    uint32_t serverIp;      // big-endian (0 = use connection IP)
    // Followed by HTTP request string (variable length)

    // Accessors (convert from network byte order)
    uint32_t getReplayGain() const { return ntohl(replayGainOrInterval); }
    uint16_t getServerPort() const { return ntohs(serverPort); }
    uint32_t getServerIp() const { return ntohl(serverIp); }
} __attribute__((packed));

static_assert(sizeof(StrmCommand) == 24, "StrmCommand must be 24 bytes");

// ============================================
// Server -> Client: audg command payload
// ============================================

struct AudgCommand {
    uint32_t oldGainLeft;   // big-endian, legacy 0-128
    uint32_t oldGainRight;  // big-endian, legacy 0-128
    uint8_t dvc;            // digital volume control flag
    uint8_t preamp;         // 255=silent, 0=full
    uint32_t newGainLeft;   // big-endian, 16.16 fixed-point
    uint32_t newGainRight;  // big-endian, 16.16 fixed-point
    // Optional: uint32_t sequence (v7.0+)

    uint32_t getNewGainLeft() const { return ntohl(newGainLeft); }
    uint32_t getNewGainRight() const { return ntohl(newGainRight); }
} __attribute__((packed));

static_assert(sizeof(AudgCommand) == 18, "AudgCommand must be 18 bytes");

// ============================================
// Helper: Build HELO payload
// ============================================

// HELO payload layout (36 bytes fixed + variable capabilities)
struct HeloPayload {
    uint8_t deviceId;       // DEVICE_ID_SQUEEZESLAVE recommended
    uint8_t revision;       // firmware revision (0)
    uint8_t mac[6];         // MAC address
    uint8_t uuid[16];       // UUID (all zeros is fine)
    uint16_t wlanChannels;  // big-endian, 0
    uint32_t bytesRecvHi;   // big-endian
    uint32_t bytesRecvLo;   // big-endian
    char language[2];       // "en"
    // Followed by capabilities string (variable)
} __attribute__((packed));

static_assert(sizeof(HeloPayload) == 36, "HeloPayload must be 36 bytes");

// ============================================
// Helper: Build STAT payload
// ============================================

// STAT payload: event code (4 bytes) + 49 bytes = 53 bytes total
struct StatPayload {
    char eventCode[4];          // e.g. "STMt"
    uint8_t crlf;               // 0
    uint8_t masInit;            // 0
    uint8_t masMode;            // 0
    uint32_t streamBufSize;     // big-endian
    uint32_t streamBufFull;     // big-endian
    uint32_t bytesRecvHi;       // big-endian
    uint32_t bytesRecvLo;       // big-endian
    uint16_t signalStrength;    // big-endian, 0xFFFF for wired
    uint32_t jiffies;           // big-endian, ms since startup
    uint32_t outputBufSize;     // big-endian
    uint32_t outputBufFull;     // big-endian
    uint32_t elapsedSeconds;    // big-endian
    uint16_t voltage;           // big-endian, 0
    uint32_t elapsedMs;         // big-endian
    uint32_t serverTimestamp;   // big-endian, echo back
    uint16_t errorCode;         // big-endian, 0
} __attribute__((packed));

static_assert(sizeof(StatPayload) == 53, "StatPayload must be 53 bytes");

// ============================================
// Utility: convert sample rate char to Hz
// ============================================

inline uint32_t sampleRateFromCode(char code) {
    switch (code) {
        case '0': return 11025;
        case '1': return 22050;
        case '2': return 32000;
        case '3': return 44100;
        case '4': return 48000;
        case '5': return 8000;
        case '6': return 12000;
        case '7': return 16000;
        case '8': return 24000;
        case '9': return 96000;
        default:  return 0;  // self-describing
    }
}

inline uint32_t sampleSizeFromCode(char code) {
    switch (code) {
        case '0': return 8;
        case '1': return 16;
        case '2': return 20;
        case '3': return 24;
        case '4': return 32;
        default:  return 0;  // self-describing
    }
}

#endif // SLIM2DIRETTA_SLIMPROTO_MESSAGES_H
