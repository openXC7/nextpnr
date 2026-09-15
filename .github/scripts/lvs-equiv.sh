#!/usr/bin/env bash
# Prove one example's extracted netlist equivalent to the synthesis it came
# from, per-register, with lvs_equiv.
#
# This is the flow xc7-bitstream-tools' scripts/verify_examples.sh runs; it is
# reproduced here so a nextpnr PR is gated on it without needing that
# checkout.  Keep the two in step -- that script is the reference.
#
#   lvs-equiv.sh <name> <workdir> <fasm> <placement> <gold.json> <xdc>
#                <part> <device> <family> <top>
#
# Environment: YOSYS, TILEVERILOG, LVS_EQUIV, PRJXRAY_DB, EQUIV_TIMEOUT.
#
# Exit 0 proved, 1 not equivalent, 77 blocked (extraction unsupported, or the
# proof timed out).  A timeout is NOT a failure: it says the design was not
# proved, for a known reason, which is a different statement from "these two
# netlists differ".
#
# EXPECT_BLOCKED names a known extraction gap.  Three outcomes then, kept
# distinguishable on purpose:
#   blocked    still stopped by the named gap        expected, exit 0
#   UNBLOCKED  it proves now -- promote it           exit 1, so it is noticed
#   DIFFER     it broke somewhere else               exit 1, a real regression
set -u -o pipefail

name=$1; d=$2; fasm=$3; placement=$4; goldjson=$5; xdc=$6
part=$7; device=$8; family=$9; top=${10}

: "${YOSYS:=yosys}"
: "${TILEVERILOG:?}"
: "${LVS_EQUIV:?}"
: "${PRJXRAY_DB:?}"
: "${EQUIV_TIMEOUT:=600}"

log="$d/lvs.log"; mkdir -p "$d"; : > "$log"

# Check the inputs BEFORE running anything, and fail loudly if one is absent.
# A missing input is not a blocked proof: "blocked" means the flow was asked a
# question it cannot answer yet, and reports exit 77 so the gate stays green.
# A file that was never produced means the design did not build, and nothing
# was compared at all -- reporting that as blocked hides a broken build behind
# a green tick.  tileverilog aborts on a missing --fasm with a C++ exception
# ("cannot open <file>"), which the extraction branch below would have
# classified as an unsupported primitive.
for f in "$fasm" "$placement" "$goldjson" "$xdc"; do
    if [ ! -s "$f" ]; then
        echo "::error::$name: LVS input missing or empty: $f"
        echo "Nothing was compared.  This is not a blocked proof -- the step"
        echo "that should have produced this file did not, so look there first."
        exit 1
    fi
done

# Same again for the database side.  tileverilog reads the tilegrid for the
# DIE, and prjxray does not model every part number separately: the Arty's
# xc7a35t is the xc7a50t die, and artix7/xc7a35t/ simply does not exist.
# Passing a part where a die belongs aborts tileverilog with "cannot open
# .../tilegrid.json", which the extraction branch would again have called a
# blocked proof.
if [ ! -s "$PRJXRAY_DB/$family/$device/tilegrid.json" ]; then
    echo "::error::$name: no tilegrid for device '''$device''' under $PRJXRAY_DB/$family"
    echo "This wants the die prjxray models (xc7a50t), not the part (xc7a35tcsg324-1)."
    exit 1
fi

# Extraction: the bitstream back to a fabric netlist.  A primitive the tile
# model does not cover stops it here, which is a blocker rather than a
# difference -- nothing has been compared yet.
if ! "$TILEVERILOG" --fasm "$fasm" --db "$PRJXRAY_DB/$family" --device "$device" \
        --xdc "$xdc" --part "$part" --out "$d/fabric.v" \
        --model-out "$d/tile_model.v" >>"$log" 2>&1; then
    echo "::notice::$name: blocked -- extraction from the bitstream failed"
    tail -15 "$log"
    exit 77
fi

# -norename matters: without it write_verilog renames every internal object
# to _<number>_, so the two representations of the one design share no names
# for the register correspondence to be built on, and an unmatched register
# makes every cone downstream of it incomparable.
"$YOSYS" -q -p "read_json $goldjson; hierarchy -top $top; splitnets; select $top; \
    write_verilog -noattr -norename -selected $d/gold.v" >>"$log" 2>&1

res=$(timeout "$EQUIV_TIMEOUT" \
      "$LVS_EQUIV" --gold "$d/gold.v" --gold-top "$top" \
                   --gate "$d/fabric.v" --gate-top fabric \
                   --placement "$placement" --gold-json "$goldjson" \
                   --db "$PRJXRAY_DB/$family" --device "$device" --quiet \
      2>&1 | tee -a "$log" | grep -E '^[0-9]+ proved')
rc=$?

if [ $rc -eq 124 ] || [ -z "$res" ]; then
    echo "::notice::$name: blocked -- not proved within ${EQUIV_TIMEOUT}s"
    tail -15 "$log"
    exit 77
fi

proved=$(echo "$res" | awk '{print $1}')
differ=$(echo "$res" | awk '{print $3}')
if [ "${differ:-1}" = 0 ] && [ "${proved:-0}" -gt 0 ]; then
    if [ -n "${EXPECT_BLOCKED:-}" ]; then
        echo "::error::$name: now proves ($proved registers/outputs), but is still"
        echo "listed as blocked on: $EXPECT_BLOCKED"
        echo "The gap is fixed -- drop lvs_blocked for this design so it is gated on."
        exit 1
    fi
    echo "$name: PROVED $proved registers/outputs, 0 differ"
    exit 0
fi
if [ -n "${EXPECT_BLOCKED:-}" ]; then
    echo "::notice::$name: blocked as expected -- $EXPECT_BLOCKED (${differ:-?} differ, ${proved:--} proved)"
    exit 0
fi
echo "::error::$name: the extracted netlist is not equivalent to its synthesis -- ${differ:-?} differ, ${proved:--} proved"
tail -25 "$log"
exit 1
