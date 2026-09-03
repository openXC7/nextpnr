#!/usr/bin/env python3
"""Drop unused blackbox module declarations from a yosys JSON netlist.

synth_xilinx emits the whole Xilinx cell library as blackbox modules (~433
of them, 10 MB) regardless of what the design uses.  nextpnr only needs the
modules that are actually instantiated, so keep those and the design's own
modules and drop the rest.
"""
import json, sys

src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
mods = d["modules"]

used = set()
frontier = [k for k, v in mods.items() if not v.get("attributes", {}).get("blackbox")]
seen = set()
while frontier:
    name = frontier.pop()
    if name in seen:
        continue
    seen.add(name)
    used.add(name)
    for cell in mods.get(name, {}).get("cells", {}).values():
        t = cell["type"]
        if t in mods and t not in seen:
            used.add(t)
            frontier.append(t)

d["modules"] = {k: v for k, v in mods.items() if k in used}
json.dump(d, open(dst, "w"))
print(f"{len(mods)} -> {len(d['modules'])} modules, "
      f"{len(open(src).read())} -> {len(open(dst).read())} bytes")
