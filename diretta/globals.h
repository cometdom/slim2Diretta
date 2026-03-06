/**
 * @file globals.h
 * @brief Global variables for squeeze2diretta
 *
 * Provides globals required by DirettaSync from DirettaRendererUPnP
 */

#ifndef SQUEEZE2DIRETTA_GLOBALS_H
#define SQUEEZE2DIRETTA_GLOBALS_H

#include "LogLevel.h"

// Forward declaration for LogRing (defined in DirettaSync.h)
class LogRing;

// Global verbose flag for logging (kept for backward compatibility with DirettaSync)
extern bool g_verbose;

// Global SCHED_FIFO real-time priority for worker threads (1-99, default 50)
extern int g_rtPriority;

// Global log ring for async logging (optional, can be nullptr)
extern LogRing* g_logRing;

#endif // SQUEEZE2DIRETTA_GLOBALS_H
