# Coordinate spaces

This is the part of the library most worth understanding, because getting it
wrong produces clicks that land in the wrong place while everything appears to
work.

## The three spaces

| Space | Unit | Origin | Used by |
|---|---|---|---|
| `logical` | OS points / DIPs | top-left of the virtual desktop | all input; the default |
| `physical` | device pixels | top-left of the virtual desktop | raw captures, window-server geometry |
| `image` | pixels of the last capture | top-left of that image | coordinates read off a screenshot |

On a 2× Retina display, logical `(100, 200)` is physical `(200, 400)`. On a
Windows monitor at 150%, logical `(100, 200)` is physical `(150, 300)`.

## The mistake this prevents

```
1. Take a screenshot of a 2x display -> a 2880x1800 image
2. Downscale it to 1600 wide to fit a payload budget
3. Spot a button at (800, 500) in that image
4. Click (800, 500)
```

Step 4 is wrong twice over: once for the downscale, once for the Retina
factor. The button is at logical `(720, 450)`.

Passing the space fixes it:

```json
{ "action": "click", "at": { "x": 800, "y": 500, "space": "image" } }
```

The capture pipeline records the transform for each frame — which region of
the physical desktop it covered and what factor was applied — so image
coordinates convert back exactly.

## Mixed DPI

There is no single scale factor on a machine with a 2× laptop panel and a 1×
external monitor. Conversion therefore resolves the point against the display
that contains it, and uses *that* display's factor. Using the primary
display's factor everywhere is the classic bug, and it puts every click on the
secondary monitor in the wrong place.

A point outside every display resolves against the nearest one rather than
failing, so a coordinate a few pixels off the edge still converts sensibly.
A point far outside is rejected by `Session::resolve()` with a message naming
the actual desktop bounds — silently clicking nowhere is worse than an error.

## Platform notes

**macOS** — CGEvent takes points. A scaled Retina mode ("Looks like
1512×982" on a 3024×1964 panel) has a non-integer factor, so the pixel
dimensions come from the display mode rather than from the bounds.

**Windows** — with per-monitor DPI awareness V2 (set at startup), the API
reports physical pixels. Absolute `SendInput` coordinates are normalised
0–65535 across the **virtual** desktop, not the primary display.

**Linux** — X11 has no per-monitor scale. Precedence is `GDK_SCALE` /
`QT_SCALE_FACTOR`, then `Xft.dpi`, then 1.0. The chosen factor is reported in
`displays` so you can see what was assumed.

## Device points

A mobile device adds a third factor. An iPhone 15 Pro is 393×852 points; its
simulator renders at 3× into a window macOS then draws at 2×, and the user may
have resized it. `DeviceViewport` maps device points to host points, so device
coordinates stay stable no matter how the window is sized.
