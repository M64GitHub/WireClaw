#!/usr/bin/env bash
# WireClaw — OpenClaw tool execution helper
#
# Usage:
#   wc.sh exec <device> <tool> [json_params]  — Execute a tool on a device
#   wc.sh caps <device>                        — Query device capabilities
#   wc.sh discover                             — Find all WireClaw devices
#   wc.sh sub <device>                         — Subscribe to event stream
#
# Examples:
#   wc.sh exec wireclaw-01 led_set '{"r":255,"g":0,"b":0}'
#   wc.sh exec wireclaw-01 sensor_read '{"name":"chip_temp"}'
#   wc.sh exec wireclaw-01 device_info
#   wc.sh caps wireclaw-01
#   wc.sh discover
#   wc.sh sub wireclaw-01
#
# Environment:
#   WIRECLAW_NATS_URL  — NATS server URL (default: nats://localhost:4222)

set -euo pipefail

NATS_URL="${WIRECLAW_NATS_URL:-nats://localhost:4222}"

usage() {
    echo "Usage:"
    echo "  wc.sh exec <device> <tool> [json_params]  — Execute a tool"
    echo "  wc.sh caps <device>                        — Query capabilities"
    echo "  wc.sh discover                             — Find all devices"
    echo "  wc.sh sub <device>                         — Subscribe to events"
    exit 1
}

cmd_exec() {
    local device="${1:?exec requires <device> <tool> [json_params]}"
    local tool="${2:?exec requires <device> <tool> [json_params]}"
    local params="${3:-}"

    # Merge "tool" key into params JSON
    local payload
    if [ -n "$params" ]; then
        # Strip leading { from params, prepend {"tool":"...","
        payload="{\"tool\":\"${tool}\",${params#\{}"
    else
        payload="{\"tool\":\"${tool}\"}"
    fi

    exec nats req "${device}.tool_exec" "$payload" \
        --server="${NATS_URL}" \
        --timeout=10s
}

cmd_caps() {
    local device="${1:?caps requires <device>}"

    exec nats req "${device}.capabilities" "" \
        --server="${NATS_URL}" \
        --timeout=5s
}

cmd_discover() {
    exec nats req "_wc.discover" "" \
        --server="${NATS_URL}" \
        --replies=0 \
        --timeout=3s
}

cmd_sub() {
    local device="${1:?sub requires <device>}"

    exec nats sub "${device}.events" \
        --server="${NATS_URL}"
}

# --- Main ---

if [ $# -lt 1 ]; then
    usage
fi

case "$1" in
    exec)     shift; cmd_exec "$@" ;;
    caps)     shift; cmd_caps "$@" ;;
    discover) shift; cmd_discover "$@" ;;
    sub)      shift; cmd_sub "$@" ;;
    -h|--help) usage ;;
    *)        echo "Unknown command: $1"; usage ;;
esac
