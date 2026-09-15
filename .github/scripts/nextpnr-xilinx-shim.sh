#!/usr/bin/env bash
# Compatibility shim: translate nextpnr-xilinx CLI flags (used by
# openXC7/demo-projects' openXC7.mk) into nextpnr-himbaechel flags.
#
#   nextpnr-xilinx --chipdb <path> --xdc <file> --json <file>
#                  --fasm <out> [--seed N] [extra args...]
#
# The chipdb filename (chipdb-<die>.bin) supplies the device; both bare
# die names and part-form names are accepted by the himbaechel xilinx
# uarch's device parser.
set -euo pipefail

BIN="nextpnr-himbaechel"
if [ -n "${NEXTPNR_HIMBAECHEL_BIN:-}" ]; then
    BIN="$NEXTPNR_HIMBAECHEL_BIN"
fi

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --chipdb)
            CHIPDB="$2"; shift 2 ;;
        --xdc)
            XDC="$2"; shift 2 ;;
        --fasm)
            FASM="$2"; shift 2 ;;
        --json)
            ARGS+=("--json" "$2"); shift 2 ;;
        *)
            ARGS+=("$1"); shift ;;
    esac
done

if [ -z "${CHIPDB:-}" ]; then
    echo "shim: --chipdb is required" >&2
    exit 1
fi
DIE="$(basename "$CHIPDB" | sed 's/^chipdb-//; s/\.bin$//')"
ARGS+=("--chipdb" "$CHIPDB")
ARGS+=("--device" "$DIE")
if [ -n "${XDC:-}" ]; then
    ARGS+=("-o" "xdc=$XDC")
fi
if [ -n "${FASM:-}" ]; then
    ARGS+=("-o" "fasm=$FASM")
fi

exec "$BIN" "${ARGS[@]}"
