# computer-control (Python)

Cross-platform desktop and mobile-simulator automation.

```bash
pip install computer-control
```

```python
from computer_control import Session

with Session() as cc:
    cc.screenshot("screen.png", max_dimension=1200)
    cc.click(640, 480)
    cc.type("hello world", enter=True)
    cc.pinch(2.0, at=(700, 400))
```

The binding is pure `ctypes` over the project's C ABI — no compiled extension
and no build step, so it works on CPython and PyPy alike.

## macOS permissions

The permission belongs to **the process that loads the library**, which for a
script is the Python interpreter — not `cc`, and not the wheel. So the binary
to grant is something like:

```
/opt/homebrew/.../Python.framework/Versions/3.14/Resources/Python.app
```

`cc.batch([{"action": "permissions"}])` prints the exact path, and whether the
grant is being attributed to a parent process instead:

```python
with Session() as cc:
    report = cc.batch([{"action": "permissions"}])["steps"][0]["result"]
    print(report["executable"])                      # what to grant
    print(report.get("permissions_attributed_to"))   # e.g. "iTerm2"
```

A script run from a granted terminal usually inherits enough to work. If
element queries come back empty while `accessibility` reports granted, that
inheritance is the reason — see the main README's macOS section.

Pass `{"request": True}` to prompt for anything missing. Grants are read at
process launch, so restart Python afterwards.

## Coordinate spaces

The thing worth knowing. A point read off a screenshot is not a point you can
click on a Retina or scaled display:

```python
from computer_control import Session, Space

with Session() as cc:
    png = cc.screenshot(max_dimension=1600)   # downscaled from 2880x1800
    # Say you find a button at (800, 500) in that image:
    cc.click(800, 500, space=Space.IMAGE)     # converted for you
```

`cc.convert((x, y), Space.PHYSICAL)` converts explicitly. On a multi-monitor
setup, conversion resolves against the display containing the point, so mixed
DPI works.

## Gestures

Fidelity differs by platform and is reported rather than faked:

```python
print(cc.gesture_support("pinch"))
# {'fidelity': 'emulated', 'backend': 'cmd-scroll', 'max_fingers': 2, 'note': '...'}

cc.gesture("swipe", direction="left", fingers=3, distance=400)
cc.rotate(90)                         # raises Unsupported on macOS
cc.gesture("pinch", scale=2.0, require_native=True)   # refuse emulation
```

## Devices

```python
for d in cc.devices():
    print(d["name"], d["platform"], d["screen_points"])

with cc.open_device("iPhone 15") as phone:
    phone.tap(196, 420)               # device points, not host pixels
    phone.swipe((196, 700), (196, 200))
    phone.screenshot("phone.png")
```

## Errors

Every error carries a `remedy` naming the exact setting to change:

```python
from computer_control import PermissionDenied

try:
    cc.elements()
except PermissionDenied as e:
    print(e.message)
    print(e.remedy)   # "System Settings > Privacy & Security > Accessibility ..."
```

## Using a local build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
COMPUTER_CONTROL_LIB=$PWD/build/libcomputer_control.dylib python my_script.py
```

Full documentation: https://github.com/minhnd410/computer-control
