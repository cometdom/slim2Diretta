#!/bin/bash
# slim2diretta - Startup Wrapper Script
# This script applies process priority settings and starts slim2diretta.
# Called by systemd: slim2diretta.service
# TARGET is read from /etc/default/slim2diretta (EnvironmentFile)

set -e

# Target index from config file or optional command-line argument
TARGET_INDEX="${1:-${TARGET:-}}"
if [ -z "$TARGET_INDEX" ]; then
    echo "ERROR: No target specified. Set TARGET in /etc/default/slim2diretta" >&2
    exit 1
fi

# Process priority defaults (overridden by EnvironmentFile)
NICE_LEVEL="${NICE_LEVEL:--10}"
IO_SCHED_CLASS="${IO_SCHED_CLASS:-realtime}"
IO_SCHED_PRIORITY="${IO_SCHED_PRIORITY:-0}"
RT_PRIORITY="${RT_PRIORITY:-50}"

SLIM2DIRETTA_BIN="/usr/local/bin/slim2diretta"

# Build command with target and user options
CMD="$SLIM2DIRETTA_BIN --target $TARGET_INDEX $SLIM2DIRETTA_OPTS"

if [ -n "$RT_PRIORITY" ] && [ "$RT_PRIORITY" != "50" ]; then
    CMD="$CMD --rt-priority $RT_PRIORITY"
fi

# Build exec prefix for process priority
EXEC_PREFIX=""

# Apply nice level
if [ -n "$NICE_LEVEL" ] && [ "$NICE_LEVEL" != "0" ]; then
    EXEC_PREFIX="nice -n $NICE_LEVEL"
fi

# Apply I/O scheduling
if [ -n "$IO_SCHED_CLASS" ]; then
    # Map class name to ionice class number
    case "$IO_SCHED_CLASS" in
        realtime|1)  IONICE_CLASS=1 ;;
        best-effort|2) IONICE_CLASS=2 ;;
        idle|3)      IONICE_CLASS=3 ;;
        *)           IONICE_CLASS="" ;;
    esac

    if [ -n "$IONICE_CLASS" ]; then
        if [ "$IONICE_CLASS" = "3" ]; then
            # idle class has no priority level
            EXEC_PREFIX="ionice -c $IONICE_CLASS $EXEC_PREFIX"
        else
            EXEC_PREFIX="ionice -c $IONICE_CLASS -n ${IO_SCHED_PRIORITY:-0} $EXEC_PREFIX"
        fi
    fi
fi

# Log the command being executed
echo "════════════════════════════════════════════════════════"
echo "  Starting slim2diretta (target $TARGET_INDEX)"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  Target:         $TARGET_INDEX"
echo "  Nice level:     $NICE_LEVEL"
echo "  I/O scheduling: $IO_SCHED_CLASS (priority $IO_SCHED_PRIORITY)"
echo "  RT priority:    $RT_PRIORITY (SCHED_FIFO)"
echo ""
echo "Command:"
echo "  $EXEC_PREFIX $CMD"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute with priority settings
exec $EXEC_PREFIX $CMD
