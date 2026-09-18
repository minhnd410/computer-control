# Examples

Runnable against a local build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
export COMPUTER_CONTROL_LIB=$PWD/build/libcomputer_control.dylib   # .so on Linux
export PYTHONPATH=$PWD/bindings/python
python examples/01_capabilities.py
```

| | |
|---|---|
| `01_capabilities.py` | What this machine can do, and what is missing. Start here. |
| `02_coordinate_spaces.py` | The Retina/scaling trap, and how the space tag avoids it. |
| `03_gestures.py` | Fidelity-aware gestures; refuses rather than faking. |
| `04_device.py` | Drive an iOS simulator or Android emulator in device points. |
| `05_batch.cpp` | The C++ API and batching, for comparison with the bindings. |

Each one is read-only or confines itself to the pointer unless you pass
`--act`, so running them cannot disturb a desktop you care about.
