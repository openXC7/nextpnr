#!/usr/bin/env bash
# HP-bank parity check for the VC707 johnson design.
#
# The reference is NOT this repo's own output.  It is the IOB feature set the
# legacy nextpnr-xilinx flow (68aeeb3, prjxray-generated chipdb) produced for
# the same design, same part and same pin constraints -- an external
# implementation of the same job.  Both flows are pin-constrained to identical
# sites, so the IOB feature set is placement-independent and directly
# comparable even though the placers differ.
#
# The check does not require parity.  It requires the DIFFERENCE to stay what
# it is known to be: .github/references/vc707-johnson-iob-delta.txt records the
# features the legacy flow writes and himbaechel/xilinx currently does not.
# Closing part of the gap fails this check just as widening it does -- both are
# changes that should be seen and the recorded delta updated deliberately.
#
# Usage: check-vc707-iob-parity.sh <produced.fasm>
set -euo pipefail

# Feature names mix '.' and '_' as separators, and the two characters collate
# in the OPPOSITE order under C/C.UTF-8 (where '.' 0x2E precedes '_' 0x5F) and
# under an en_*.UTF-8 collation (which weighs punctuation differently).  So an
# unpinned sort makes the recorded delta depend on which machine recorded it:
# a delta recorded under en_GB.UTF-8 and checked on a C.UTF-8 runner reports
# adjacent lines as moved, and diff misaligns the surrounding hunks badly
# enough to show the same tile on both sides of the comparison at once.
# Pin the collation so the recorded file means the same thing everywhere.
export LC_ALL=C

record=0
if [ "${1:-}" = "--record" ]; then
    record=1
    shift
fi
fasm="${1:?usage: $0 [--record] <fasm>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
legacy="$here/references/vc707-johnson-legacy-iob.txt"
expected="$here/references/vc707-johnson-iob-delta.txt"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Both sides must be ordered by the same rule for diff to pair them up, and
# the committed legacy list was sorted whenever it was first captured -- so
# re-sort it here rather than trusting the order it carries.
sort -u "$legacy" > "$tmp/legacy.txt"
grep -E '^(L|R)IOB' "$fasm" | sort -u > "$tmp/produced.txt"
diff "$tmp/legacy.txt" "$tmp/produced.txt" | grep '^[<>]' | sort > "$tmp/delta.txt" || true

if [ "$record" = 1 ]; then
    cp "$tmp/delta.txt" "$expected"
    echo "recorded $(grep -c '^<' "$expected") missing and $(grep -c '^>' "$expected") extra features in $expected"
    exit 0
fi

if diff -u "$expected" "$tmp/delta.txt" > "$tmp/drift.txt"; then
    echo "IOB parity: delta vs legacy flow unchanged ($(grep -c '^<' "$expected") legacy features still missing)"
    exit 0
fi

echo "::error::VC707 IOB delta against the legacy flow has changed"
echo
echo "  '<' = written by the legacy flow, not by this build"
echo "  '>' = written by this build, not by the legacy flow"
echo
cat "$tmp/drift.txt"
echo
echo "If this change is intended -- e.g. the HP-bank output path now emits"
echo "OBUF_HP_BANK_GLUE and the DRIVE/STEPDOWN bits the legacy flow writes --"
echo "re-record the delta and say in the commit message which features moved:"
echo "  .github/scripts/check-vc707-iob-parity.sh --record <fasm>"
exit 1
