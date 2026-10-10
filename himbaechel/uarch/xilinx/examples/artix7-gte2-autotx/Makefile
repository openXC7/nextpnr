# No hardware actions. PREFIX contains the pinned openXC7 build/db/chipdb.
PREFIX ?= /opt/openxc7
YOSYS ?= yosys
OUT ?= build
PART = xc7a100tfgg484-2
unexport XRAY_ALLOW_MISSING_FEATURES
.PHONY: all sim
all: $(OUT)/top.bit
$(OUT)/top.json: top.v pins.xdc
	mkdir -p $(OUT)
	$(YOSYS) -Q -T -p 'read_verilog top.v; synth_xilinx -family xc7 -top openxc7_autotx -flatten -json $@' > $(OUT)/yosys.log 2>&1
$(OUT)/top.fasm: $(OUT)/top.json
	$(PREFIX)/build/nextpnr-himbaechel --device $(PART) --chipdb $(PREFIX)/chipdb-xc7a100t.bin --json $< --freq 125 -o xdc=pins.xdc -o hold-fix=8 -o fasm=$@ --write $(OUT)/routed.json --report $(OUT)/timing.json > $(OUT)/nextpnr.log 2>&1
$(OUT)/top.frames: $(OUT)/top.fasm
	PYTHONPATH=$(PREFIX)/prjxray $(PREFIX)/pyenv/bin/python $(PREFIX)/prjxray/utils/fasm2frames.py --db-root $(PREFIX)/prjxray-db/artix7 --part $(PART) $< $@ > $(OUT)/fasm2frames.log 2>&1
$(OUT)/top.bit: $(OUT)/top.frames
	$(PREFIX)/xray-build/tools/xc7frames2bit --part_file $(PREFIX)/prjxray-db/artix7/$(PART)/part.yaml --part_name $(PART) --frm_file $< --output_file $@ > $(OUT)/frames2bit.log 2>&1
sim:
	mkdir -p $(OUT)
	iverilog -g2012 -s tb -o $(OUT)/sim top.v tb.v
	vvp $(OUT)/sim
