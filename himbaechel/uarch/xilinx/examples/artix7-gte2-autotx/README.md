# Artix-7 constant-low CEB reference-buffer reproducer

On a Kasli v1.1 XC7A100T-FGG484-2, an autonomous UART transmitter built
with the unpatched openXC7 flow produces no received bytes. Removing only
`GTP_COMMON_X130Y179.IBUFDS_GTE2_Y1.IN_USE` restores transmission. The
correction in `write_ibufds_gte2` omits this feature for Artix-7 buffers whose
CEB is connected to the packed constant-ground net.

This example has no CPU, RX, memory, MMCM, PLL, or external reset. A qualified
125 MHz MGT reference on F10/E10 feeds IBUFDS_GTE2.O and one BUFG. An
initialized counter sends 0x55 as 8N1 on T16, with divisor 1085 (nominal
115207.37 baud), two extra idle bit periods, and an optional heartbeat on
T21. Outputs use LVCMOS25. The three SFP transmitters are disabled.

## Controlled physical evidence

These are retained results from 2026-10-10, using the pinned versions below.
They are not new physical tests of every later upstream revision.

| Experiment | Received data |
| --- | --- |
| Identical RTL through Vivado | 9523/9523 correct 0x55 bytes |
| Unpatched openXC7 | Zero bytes at five baud settings |
| Vivado bitstream decoded to FASM and rebuilt by strict X-Ray | 9524/9524 correct bytes |
| Open FASM with only IN_USE removed | 9523/9523 correct bytes |
| Source-rebuilt nextpnr with the scoped correction | 9677/9677 correct bytes |

The five baud settings were 115200, 57600, 230400, 62500, and 125000.
The UART captures used the carrier's FTDI channel. No independent scope,
logic analyzer, or second UART adapter was used; reference-clock interruption
is an inferred mechanism, not a measured electrical waveform.

The archived FASM files have identical placement/routing and differ by exactly
one feature. The corresponding configuration data differs at frame
`0x0002199c`, word 49, bit 11 (set in fail, clear in pass). Frame ECC is updated
by bitstream generation. See `artifacts/frame-delta.json`.

For this submission, nextpnr was rebuilt from main
`8006fbc6f8f7fa3ae5ff0409171ffe838abb2f93` plus the correction. The example
passed synthesis, placement/routing, strict FASM-to-frames, and bitstream
generation using the retained pinned database/chipdb. Its generated frames
are byte-for-byte identical to the physically tested source-fixed probe:
SHA-256 `b5af71ae3faf4b3da20fa2f07e9e33c32710375b861725f0f209b4498a44d386`.
The UART simulation also passed. No new hardware programming was performed
for this submission.

The same scoped source correction also restored the unchanged PicoRV32 system
at both 100 MHz (MMCM) and 62.5 MHz (no MMCM): GET_INFO, all six ML-DSA-65 /
ML-KEM-768 interoperability operations against OpenSSL 3.5.8 (38/38 checks
per frequency), and 9/9 hardware vector/error/reset tests per frequency passed.
These larger systems are supporting evidence; this example is the minimal
reproducer. The no-CPU UART/ROM probe read all 131072 initialized bytes exactly.

Only FPGA SRAM was programmed. The final known-good Ethernet restoration
passed. One intermediate restoration initially failed because autonomous UART
bytes remained queued; a subsequent recovery passed after draining the queue.
The original failure was retained. No Flash, OTP, EEPROM, boot-mode, or security
settings were written.

## Scope and remaining questions

The database's `063-gtp-common-conf` fuzzer leaves CEB unconnected and labels
primitive presence as IN_USE. This does not establish the required value when
CEB is explicitly low. Vivado leaves IN_USE clear in this tested case; the
other reference-buffer configuration fields match.

The correction retains existing behavior for other device families and for
high, dynamic, or unconnected CEB. Those cases, Artix parts other than this
XC7A100T, and designs also using the GTP transceivers need further physical
characterization. This is a scoped, experimentally supported correction, not
a claim about the bit's universal electrical semantics.

## Reproduce offline

The Makefile performs no hardware programming. Use a carrier with matching
pinout and 2.5 V I/O supplies before adapting any hardware test.

Pinned versions used for the physical evidence:

- nextpnr: `3e5c2cdd256745fb5adb93f3ffe8c3795b1e7abf`, plus this correction.
- prjxray-db: `517d66a383676cb971177ea92b0ff3b6ea6e8690`.
- prjxray: `ed3331c6200f421164101388759fc2860b0f5634`.
- Yosys: `0.69+272`, commit `230fb23f8`.

The included flake.lock pins the development dependencies. It does not itself
build the complete openXC7 toolchain. Prepare a prefix with `build/nextpnr-himbaechel`,
`chipdb-xc7a100t.bin`, `prjxray`, `prjxray-db`, `pyenv`, and `xray-build`.
Choose the recorded Yosys explicitly when the Nix-provided version differs.

```sh
nix develop --command make sim
nix develop --command make PREFIX=/path/to/unpatched/openxc7 \
    YOSYS=/path/to/pinned/yosys OUT=fail
# Rebuild nextpnr with this correction in a separate prefix.
nix develop --command make PREFIX=/path/to/patched/openxc7 \
    YOSYS=/path/to/pinned/yosys OUT=pass
```

`XRAY_ALLOW_MISSING_FEATURES` is explicitly unset. No DRC or missing-feature
errors are suppressed. Vivado is not used in this open bitstream pipeline.

To inspect the retained controlled delta without a toolchain:

```sh
diff -u artifacts/fail.fasm artifacts/pass.fasm
sha256sum -c SHA256SUMS
gzip -dc artifacts/fail.bit.gz | sha256sum
gzip -dc artifacts/pass.bit.gz | sha256sum
```

The diff contains only the removed IN_USE feature. Expected uncompressed
bitstream hashes are in `artifacts/IMAGE_SHA256.json`. The public images have
the local path removed from the design-name header; their configuration
payloads are byte-for-byte identical to the physically tested originals.
The manifest records original and public hashes plus payload hashes.
Bitstream headers can vary with generation time; frame comparison establishes
the one-bit result.
Hardware identifiers, network addresses, and laboratory recovery images are
not included.
