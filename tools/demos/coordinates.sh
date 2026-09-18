#!/usr/bin/env sh
# The Retina trap: a pixel read off a downscaled screenshot is not a click.
./build/cc screenshot --out /tmp/_demo.png --max_dimension 1000 >/dev/null 2>&1
./build/cc displays --raw 2>/dev/null | python3 -c '
import sys, json
d = json.load(sys.stdin)["result"]["displays"][0]
lw, lh = d["bounds_logical"]["w"], d["bounds_logical"]["h"]
pw, ph = d["bounds_physical"]["w"], d["bounds_physical"]["h"]
print("  display    %.0fx%.0f pt  @%gx  =  %.0fx%.0f px" % (lw, lh, d["scale"], pw, ph))
print("  screenshot downscaled to 1000 px wide")
print("")
img_to_phys = pw / 1000.0
phys_to_log = lw / pw
lx = 500 * img_to_phys * phys_to_log
ly = 300 * img_to_phys * phys_to_log
print("  a button at (500, 300) in that image is really at")
print("      logical  (%.0f, %.0f)   <- what a click needs" % (lx, ly))
print("  clicking the raw pixel misses by (%.0f, %.0f) points" % (lx - 500, ly - 300))
print("")
print("  cc click --at 500,300@image   converts it for you")
'
