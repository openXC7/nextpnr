# Reusable Sonata board test harness for himbaechel/xilinx.
#
# Usage:
#   make DESIGN=blinky_sonata TOP=blinky_sonata XDC=blinky_sonata.xdc fasm
#   make DESIGN=blinky_sonata TOP=blinky_sonata XDC=blinky_sonata.xdc bit
#   make DESIGN=blinky_sonata TOP=blinky_sonata XDC=blinky_sonata.xdc uf2       # slot 1 (default)
#   make DESIGN=blinky_sonata TOP=blinky_sonata XDC=blinky_sonata.xdc SLOT=2 uf2 # slot 2
#   make TESTS="blinky_sonata other_design" all
#   make TESTS="blinky_sonata other_design" all-uf2  # all as UF2 slot 1
#
# The board-specific defaults are intentionally fixed so every Sonata test uses
# the same device, XDC, and router settings.

DEVICE ?= xc7a50tcsg324-1
ROUTER ?= router1
NEXTPNR ?= ../../../../../build/nextpnr-himbaechel
YOSYS ?= yosys
DESIGN ?= blinky_sonata
TOP ?= $(DESIGN)
XDC ?= $(DESIGN).xdc
BITGEN ?= ../bitgen_xray.sh

# Sonata UF2 configuration
# Three available slots: 1 (0x00000000), 2 (0x10000000), 3 (0x20000000)
SLOT ?= 1
SLOT_ADDR = $(word $(SLOT),0x00000000 0x10000000 0x20000000)
SONATA_FAMILY_ID = 0x6ce29e6b

TESTS ?= blinky_sonata johnson_sonata

.PHONY: all clean fasm bit uf2 all-uf2

all: $(foreach t,$(TESTS),$(t).bit)

all-uf2: $(foreach t,$(TESTS),$(t).bit.slot$(SLOT).uf2)

$(DESIGN).json: $(DESIGN).v
	$(YOSYS) -p "synth_xilinx -flatten -abc9 -nobram -arch xc7 -top $(TOP); write_json $(DESIGN).json" $(DESIGN).v

fasm: $(DESIGN).fasm

$(DESIGN).fasm: $(DESIGN).json $(XDC)
	$(NEXTPNR) --device $(DEVICE) -o xdc=$(XDC) --json $(DESIGN).json -o fasm=$(DESIGN).fasm --router $(ROUTER)

bit: $(DESIGN).bit

$(DESIGN).bit: $(DESIGN).fasm
	$(BITGEN) $(DEVICE) $(DESIGN).fasm $(DESIGN).bit

uf2: $(DESIGN).bit.slot$(SLOT).uf2

$(DESIGN).bit.slot$(SLOT).uf2: $(DESIGN).bit
	python3 uf2conv.py $(DESIGN).bit -o $@ -b $(SLOT_ADDR) -f $(SONATA_FAMILY_ID)

clean:
	rm -f *.json *.fasm *.bit *.frames *.slot*.uf2
