# Sonata board example

This example targets the lowRISC Sonata ONE board, which uses the Artix-7 part
`xc7a50tcsg324-1`.

## Files

- `blinky_sonata.v` — a minimal LED-chaser design
- `blinky_sonata.xdc` — board pin constraints for the 25 MHz oscillator and LEDs
- `Makefile` — synthesize, place, route, and optionally produce Sonata UF2 images

## Important note

Use `--router router1` for this board. The Sonata clock path through the
regional-clock network is not legal with `router2` in the current himbaechel
xilinx port, and the board notes explicitly require `router1` to route the
25 MHz oscillator through the correct CCIO/CMT/HROW path.

## Run it

From the repo root, provide the freshly built binary and Project X-Ray paths:

```sh
cd himbaechel/uarch/xilinx/examples/sonata
export PATH="/path/to/build:$PATH"
export PRJXRAY=/path/to/prjxray
export PRJXRAY_DB=/path/to/prjxray-db
export PYTHONPATH="$PRJXRAY${PYTHONPATH:+:$PYTHONPATH}"
make DESIGN=blinky_sonata TOP=blinky_sonata XDC=blinky_sonata.xdc uf2
```

## Building UF2 images

The Sonata board supports three FPGA configuration slots:

```sh
# Slot 1 (default, address 0x00000000)
make DESIGN=johnson_sonata TOP=johnson_sonata XDC=johnson_sonata.xdc uf2
# → johnson_sonata.bit.slot1.uf2

# Slot 2 (address 0x10000000)
make DESIGN=johnson_sonata TOP=johnson_sonata XDC=johnson_sonata.xdc SLOT=2 uf2
# → johnson_sonata.bit.slot2.uf2

# Slot 3 (address 0x20000000)
make DESIGN=johnson_sonata TOP=johnson_sonata XDC=johnson_sonata.xdc SLOT=3 uf2
# → johnson_sonata.bit.slot3.uf2

# Build all designs as slot 1 UF2
make all-uf2
```

The Makefile expects the usual Xilinx tooling to be available:

- `yosys`
- `nextpnr-himbaechel`
- `PRJXRAY` and `PRJXRAY_DB` for `../bitgen_xray.sh`

That is the same flow used by the Artix-7 examples in this directory.

## Uploading to Sonata

The Sonata board auto-mounts as a USB volume on macOS when connected. Simply copy the `.bit.slotN.uf2` file to trigger programming — the board's behavior is determined by the UF2 content and slot selection.

```sh
# Connect/power-cycle the Sonata board
# It will auto-mount as /Volumes/SONATA

cp -X johnson_sonata.bit.slot1.uf2 /Volumes/SONATA/
sync
```

The board will automatically program the FPGA with your design.

## Johnson adaptation

The Sonata version of the VC707 Johnson counter uses a 25-bit maximal-length
LFSR for the equivalent cadence at 25 MHz, and swaps the board-specific signals to the Sonata
ports:

- clock: `mainClk` on pad `P15`
- outputs: `usrLed[7:0]` on the Sonata LED pins
- no VC707 differential clock or reset input is retained; the Sonata board
  uses a single 25 MHz oscillator and the LED pattern is generated entirely from
  that source.
