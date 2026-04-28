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

# Advanced network config (ethtool link tuning)
TARGET_INTERFACE="${TARGET_INTERFACE:-}"
TARGET_SPEED="${TARGET_SPEED:-100}"
TARGET_DUPLEX="${TARGET_DUPLEX:-full}"

# IRQ affinity for the target NIC (away from --cpu-audio core)
IRQ_INTERFACE="${IRQ_INTERFACE:-}"
IRQ_CPUS="${IRQ_CPUS:-}"

# SMT control: on / off / forceoff / empty (no change)
SMT="${SMT:-}"

# Apply system-level network and CPU tuning BEFORE launching slim2diretta so
# any subsequent --cpu-audio / --cpu-other pinning sees the right topology.

# Force NIC speed/duplex via ethtool (audiophile link tuning)
if [ -n "$TARGET_INTERFACE" ]; then
    if command -v ethtool >/dev/null 2>&1; then
        echo "Set advanced target network settings: $TARGET_INTERFACE -> ${TARGET_SPEED}Mbit/${TARGET_DUPLEX}-duplex"
        ethtool -s "$TARGET_INTERFACE" speed "$TARGET_SPEED" duplex "$TARGET_DUPLEX"
        sleep 1
    else
        echo "WARNING: TARGET_INTERFACE set but ethtool is not installed — skipping link tuning." >&2
    fi
fi

# IRQ affinity: pin all IRQs whose name contains any of the interfaces listed
# in $IRQ_INTERFACE (comma-separated, e.g. "enp1s0,enp2s0") to the CPU list
# $IRQ_CPUS. Useful to keep network interrupts off the audio worker core,
# including setups with separate NICs for the upstream source and the Diretta
# target. Some IRQs (managed/MSI-X) are read-only — those are counted as
# "skipped".
if [ -n "$IRQ_INTERFACE" ] && [ -n "$IRQ_CPUS" ]; then
    pinned=0
    skipped=0
    IFS=',' read -ra IRQ_IFACE_LIST <<< "$IRQ_INTERFACE"
    for iface in "${IRQ_IFACE_LIST[@]}"; do
        iface=$(echo "$iface" | tr -d ' ')
        [ -z "$iface" ] && continue
        while IFS= read -r line; do
            irq=$(echo "$line" | awk -F: '{print $1}' | tr -d ' ')
            if [ -n "$irq" ] && [ -e "/proc/irq/$irq/smp_affinity_list" ]; then
                if echo "$IRQ_CPUS" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null; then
                    pinned=$((pinned + 1))
                else
                    skipped=$((skipped + 1))
                fi
            fi
        done < <(grep -F "$iface" /proc/interrupts)
    done
    echo "IRQ affinity for $IRQ_INTERFACE -> CPU(s) $IRQ_CPUS: $pinned pinned, $skipped skipped (managed/read-only)"
fi

# SMT (Hyper-Threading) toggle. System-wide; non-persistent across reboots
# unless `nosmt` is also added to the GRUB cmdline.
if [ -n "$SMT" ]; then
    SMT_CTRL="/sys/devices/system/cpu/smt/control"
    case "$SMT" in
        on|off|forceoff)
            if [ -w "$SMT_CTRL" ]; then
                current=$(cat "$SMT_CTRL" 2>/dev/null || echo "?")
                if [ "$current" != "$SMT" ]; then
                    if echo "$SMT" > "$SMT_CTRL" 2>/dev/null; then
                        echo "SMT: $current -> $SMT"
                    else
                        echo "WARNING: SMT change to '$SMT' refused (BIOS lock or kernel-restricted)" >&2
                    fi
                else
                    echo "SMT already $current — no change"
                fi
            else
                echo "WARNING: SMT control not available at $SMT_CTRL" >&2
            fi
            ;;
        *)
            echo "WARNING: invalid SMT value '$SMT' — use on/off/forceoff or leave empty" >&2
            ;;
    esac
fi

SLIM2DIRETTA_BIN="/usr/local/bin/slim2diretta"

# Build command with target and user options
# Note: SLIM2DIRETTA_OPTS may contain quoted values (e.g. --name "My Player"),
# so we use eval to preserve shell quoting when constructing the command.
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
# Use eval to preserve quoted arguments in SLIM2DIRETTA_OPTS (e.g. --name "My Player")
eval exec $EXEC_PREFIX $CMD
