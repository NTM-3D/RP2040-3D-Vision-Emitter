# RP2040 3D Vision Emitter

RP2040 3D Vision Emitter is inspired by the original [3DVisionAVR](https://github.com/lukis101/3DVisionAVR) and reimplements it on the RP2040 microcontroller. What began as a port has matured into an improved and more robust version that resolves many of the issues found in the original firmware while expanding its capabilities in regards to the 3D Vision driver mode.  

**Only driver mode and 120 Hz refresh rate is supported.**

## Features

- **NVIDIA 3D Vision Emitter compatibility**: Emulates the NVIDIA 3D Vision emitter
- **IR Frame Engine**: Implements the 3D Vision IR protocol with RP2040 hardware-timed scheduling for accurate frame timing at 120 Hz
- **IR protocol support**: Included are 3D Vision, Samsung, XPAND, Sharp, Sony, and Panasonic. 3D Vision is the active one and the only one that has been tested.
- **Status LED States**: Built-in WS2812B RGB status indication (see [Status LED](#status-led) below)

## Hardware

### GPIO pinout

| Function | GPIO |
|----------|------|
| IR output | GPIO2 |
| Built-in RGB status LED (WS2812B) | GPIO16 |

## Status LED

The RP2040-Zero's built-in WS2812B RGB LED reflects the current emitter state:

| State | Color | Description |
|-------|-------|-------------|
| Disconnected | 🔴 Red | No USB connection to the host |
| Idle | 🔵 Blue | Connected but emitter not active |
| 3D active | 🟢 Green | Actively emitting IR sync frames |

### IR output circuit

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

You can also just connect the IR LED directly between GPIO2 and GND. You don't even have to solder it, just twist the the legs so it has a good connection. The range won't be as good and the LED might not live as long but it works. See below for examples of both variants.

### Hardware images
<p>
  <a href="Images/advanced.jpg"><img src="Images/advanced.jpg" width="250"></a>
  <a href="Images/simple_active.jpg"><img src="Images/simple_active.jpg" width="250"></a>
  <a href="Images/simple_idle.jpg"><img src="Images/simple_idle.jpg" width="250"></a>
</p>

## Building

### Prerequisites

- Windows 10 or later
- CMake
- Ninja
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- `pico-sdk` (with submodules)

### Quick build (Recommended)

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

### Manual build

```bat
set PICO_SDK_PATH=C:\path\to\pico-sdk
cmake -S . -B build -G Ninja
cmake --build build -j
```

## Flashing

The RP2040 is flashed by copying the `.uf2` file onto the board while it is in bootloader mode.

1. Hold the BOOT button on the RP2040-Zero.
2. While holding BOOT, connect the board to your PC via USB (or press and release RESET if already connected).
3. Release the BOOT button. The board will appear as a USB mass storage device named `RPI-RP2`.
4. Copy `RP2040_3D_Vision_Emitter-*.uf2` onto the `RPI-RP2` drive.
5. The board will automatically reboot and start running the firmware.

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
