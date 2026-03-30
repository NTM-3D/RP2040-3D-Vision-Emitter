# RP2040 3D Vision Emitter

RP2040 3D Vision Emitter is a reimplementation of the original [3DVisionAVR](https://github.com/lukis101/3DVisionAVR) project on the RP2040 microcontroller.  
**Currently only driver mode and 120 Hz frame rate is supported.**

> ⚠️ **Disclaimer**: This firmware is a work in progress and is not guaranteed to be fully stable. Occasional flickering has been observed and is a known issue. If you have suggestions, fixes, or improvements, please open an issue or pull request — all contributions are very welcome!

---

## Features

- **NVIDIA 3D Vision Emitter compatibility**: Emulates the NVIDIA 3D Vision emitter
- **IR Frame Engine**: Implements the 3D Vision IR protocol with RP2040 hardware-timed scheduling for accurate frame timing at 120 Hz
- **Status LED States**: Built-in WS2812B RGB status indication (see [Status LED](#status-led) below)

---

## Hardware

### GPIO Pinout

| Function | GPIO |
|----------|------|
| IR output | GPIO2 |
| Eye LED output | GPIO3 |
| Active LED output | GPIO4 |
| Standby LED output | GPIO5 |
| Built-in RGB status LED (WS2812B) | GPIO16 |

### IR Output Circuit

The IR LED is driven via GPIO2 through a 2N3904 NPN transistor. The 120Ω resistor is in series with the IR LED to limit the LED current. GPIO2 connects directly to the base of the transistor to switch it.

```
5V ─[IR LED]─[120Ω]─┐
					│
					│
               ┌────┤ Collector
GPIO2 ──────── ┤ Base  2N3904
               └────┤ Emitter
                    │
                   GND
```
---

## Status LED

The RP2040-Zero's onboard WS2812B RGB LED (GPIO16) reflects the current emitter state:

| State | Color | Description |
|-------|-------|-------------|
| Disconnected | 🔴 Red | No USB connection to the host |
| Idle | 🔵 Blue | Connected but emitter not active |
| 3D active | 🟢 Green | Actively emitting IR sync frames |
| Holdover active | 🟠 Orange | Running a synthetic holdover stream while waiting for the next frame signal |

---

## Building

### Prerequisites

- Windows 10 or later
- CMake
- Ninja
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- `pico-sdk` (with submodules)

### Quick Build (Recommended)

Run:

```bat
build.bat
```

For a clean rebuild:

```bat
build.bat -clean
```

Output:

- `build/RP2040_3D_Vision_Emitter.uf2`

### Manual Build

```bat
set PICO_SDK_PATH=C:\path\to\pico-sdk
cmake -S . -B build -G Ninja
cmake --build build -j
```

---

## Credits

### Project and community references

- https://github.com/lukis101/3DVisionAVR
- https://github.com/b3nn/3DVisionAVR/tree/fix-3dvision-irprotocol
- https://www.mtbs3d.com/forum/viewtopic.php?p=195727&sid=c176cb40d57d5dec5327ff2f1753d45c#p195727

### Libraries and tools used

- **Raspberry Pi Pico SDK**
  - Repository: https://github.com/raspberrypi/pico-sdk
  - License: BSD 3-Clause

- **TinyUSB** (used through pico-sdk)
  - Repository: https://github.com/hathach/tinyusb
  - License: MIT

- **LUFA** (original AVR project dependency and reference implementation)
  - Website: http://www.lufa-lib.org/
  - License: MIT-style LUFA license

- **CMake**
  - Website: https://cmake.org
  - License: BSD 3-Clause

- **Ninja**
  - Repository: https://github.com/ninja-build/ninja
  - License: Apache License 2.0

---

For troubleshooting, suggestions, or questions, open an issue in this repository.