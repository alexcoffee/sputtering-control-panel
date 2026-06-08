# Sputtering Control System

Firmware for Raspberry Pi Pico to control a DIY sputtering machine.

## Sputtering System Overview

A sputtering system is a machine used to coat objects with a very thin layer of material.
It works inside a vacuum chamber where a gas plasma knocks atoms off a solid material (called a target) and deposits
them onto another surface (the substrate).
This process is commonly used in electronics, optics, and materials research.

### Vacuum System

The vacuum system removes air from the chamber.

- Vacuum chamber: where the coating process happens
    - [18" Pyrex Bell Jar](https://www.greatglas.com/PyrexBellJars.htm)
- Roughing pump: removes most of the air
    - [Edwards E2M1.5](https://us.my.edwardsvacuum.com/en_US/USD/c/E2M1-5/p/A37132902)
- High-vacuum pump: creates very low pressure
    - [Varian TV70](https://www.ajvs.com/varian-tv-70-lp-turbo-pump-20349)
- Pirani gauge: measures rough vacuum up to 0.4 mTorr
    - [INFICON PSG500](https://www.inficon.com/en/products/vacuum-gauge-and-controller/psg50x-psg51x)
- Ion gauge: measure deep vacuum up to 0.0000008 mTorr
    - [Edwards AIGX-S](https://www.idealvac.com/en-us/Edwards-AIGX-S-Active-ION-Gauge-NW25-KF25-10-2-to-10-10-Torr/pp/P107159?srsltid=AfmBOooh7RYUk0mnEudKmBwehVt009TX-aN-QQBIE8pq7O3lhaWI1Nxi)

### Gas System

A small amount of Argon is added to the chamber to create plasma.

- Mass flow controller: precisely control gas flow.
    - [MKS MFC](https://www.mks.com/f/gm50a-mass-flow-controller)

### High Voltage System

Provides the power needed to create and maintain the plasma.
Main parts:

- Transformer:
    - pulled from a microwave oven
    - provides 2000V AC
    - convert to 500V to 1KV DC
- Variac: regulates the voltage to the transformer

### This project controls different parts of the sputtering system.

Instead of a single board computer controlling all the instruments, each instrument has its own smaller board.
Each module uses a Raspberry Pi Pico and communicates with the other modules via CAN bus.
The modules snap together via a 6-pin magnetic pogo connector.

# Building Instructions (on Linux)

### Toolchain

Install the ARM GNU toolchain:

```bash
# Ubuntu/Debian:
sudo apt install gcc-arm-none-eabi build-essential cmake
```

### Clone

Clone with submodules:

```bash
git clone --recurse-submodules git@github.com:alexcoffee/sputtering-control-panel.git
cd sputtering-control-panel
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init --recursive
```

This is required so dependencies like `pico-sdk/lib/tinyusb` are available to CMake.

## Build

Use `tools/build.sh` to build one module (default: `roughing_pump`):

```bash
./tools/build.sh pirani
```

Or use CMake directly:

```bash
cmake -S . -B build -DSCP_MODULE_TARGET=pirani
cmake --build build
```

## Flash via picotool

Install picotool:

```bash
sudo apt install picotool
```

Then flash a module:

```bash
picotool load cmake-build-debug-eabi/modules/scp_pirani.uf2
```

To flash a specific Pico when multiple are connected, use `--serial`:

```bash
picotool load --serial RPI-RP2/1234567890 cmake-build-debug-eabi/modules/scp_pirani.uf2
```

## GPIO Pin Audit

GPIO assignments live in each module's `src/main.c` as a `g_gpio_assignments` table of `{ signal, gpio }`.

The build now runs an automatic collision check before compiling module targets:

```
cmake --build <build-dir> --target check_module_pins
```

To print an inverse pin map (used and available GPIOs per module):

```
python3 tools/check_module_pins.py --root .
```

## Access serial port on Pico via USB

```
screen /dev/ttyACM0 115200
Ctrl+A, then k
```

## Flash Modules Over CAN (USB <-> CAN Bridge)

Build and flash the `usb_can_bridge` firmware onto the bridge Pico:

```bash
./tools/build.sh usb_can_bridge
./tools/auto_flash.sh usb_can_bridge
```

Then flash a target module over CAN with:

```bash
python3 tools/can_flash.py --port /dev/ttyACM0 --bin cmake-build-debug-eabi/modules/scp_pirani.bin --target-id 2
```

Notes:

- The bridge forwards fixed-size binary USB packets to CAN for low overhead.
- Flash transport uses CAN message IDs `0x300 + module_id` (control), `0x340 + module_id` (data), and
  `0x380 + module_id` (status).
