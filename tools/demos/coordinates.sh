#!/usr/bin/env sh
# The Retina trap: a pixel read off a downscaled screenshot is not a click.
./build/computer-control-mcp --doctor 2>/dev/null | python3 -c '
import sys, re
text = sys.stdin.read()
m = re.search(r"\[0\] (.+?)\s+(\d+)x(\d+) pt @([\d.]+)x = (\d+)x(\d+) px", text)
name, lw, lh, scale, pw, ph = m.group(1).strip(), *map(float, m.groups()[1:])
print("  display    %.0fx%.0f pt  @%gx  =  %.0fx%.0f px" % (lw, lh, scale, pw, ph))
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
print("  click {\"x\":500,\"y\":300,\"space\":\"image\"}   converts it for you")
'
