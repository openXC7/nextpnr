#!/usr/bin/env bash
# Compatibility shim: provide the legacy `fasm2frames` entry point expected by
# openXC7/demo-projects' openXC7.mk, backed by the pinned prjxray checkout.
set -euo pipefail

if [ -z "${PRJXRAY_DIR:-}" ]; then
    echo "shim: PRJXRAY_DIR is required" >&2
    exit 1
fi

exec python3 "$PRJXRAY_DIR/utils/fasm2frames.py" "$@"
