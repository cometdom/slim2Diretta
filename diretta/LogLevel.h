/**
 * @file LogLevel.h
 * @brief Centralized log level system for squeeze2diretta
 *
 * Provides 4 log levels (ERROR, WARN, INFO, DEBUG) with runtime filtering
 * via g_logLevel. These macros are always active regardless of NOLOG.
 * NOLOG only disables SDK internal logging (DIRETTA_LOG in DirettaSync.h).
 *
 * Usage:
 *   LOG_ERROR("something failed: " << reason);
 *   LOG_WARN("buffer low: " << pct << "%");
 *   LOG_INFO("Playback started");
 *   LOG_DEBUG("[Component] detailed message");
 */

#ifndef SQUEEZE2DIRETTA_LOGLEVEL_H
#define SQUEEZE2DIRETTA_LOGLEVEL_H

#include <iostream>

enum class LogLevel { ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3 };

extern LogLevel g_logLevel;

#define LOG_ERROR(x) do { \
    if (g_logLevel >= LogLevel::ERROR) { std::cerr << x << std::endl; } \
} while(0)
#define LOG_WARN(x) do { \
    if (g_logLevel >= LogLevel::WARN) { std::cout << "[WARN] " << x << std::endl; } \
} while(0)
#define LOG_INFO(x) do { \
    if (g_logLevel >= LogLevel::INFO) { std::cout << x << std::endl; } \
} while(0)
#define LOG_DEBUG(x) do { \
    if (g_logLevel >= LogLevel::DEBUG) { std::cout << x << std::endl; } \
} while(0)

#endif // SQUEEZE2DIRETTA_LOGLEVEL_H
