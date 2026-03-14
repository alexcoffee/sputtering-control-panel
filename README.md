# sputtering-control-system

A control system for a sputtering machine.

## Sputtering System Overview
A sputtering system is a machine used to coat objects with a very thin layer of material. 
It works inside a vacuum chamber where a gas plasma knocks atoms off a solid material (called a target) and deposits them onto another surface (the substrate). 
This process is commonly used in electronics, optics, and materials research.

### Vacuum System
The vacuum system removes air from the chamber.
- Vacuum chamber: where the coating process happens
  - [18" Pyrex Bell Jar](https://www.greatglas.com/PyrexBellJars.htm)
- Roughing pump: removes most of the air
  - [Edwards E2M1.5](https://us.my.edwardsvacuum.com/en_US/USD/c/E2M1-5/p/A37132902)
- High-vacuum pump: creates very low pressure
  - [Varian TV70](https://www.ajvs.com/varian-tv-70-lp-turbo-pump-20349)
- Pressure gauges: measure the vacuum level.
  - Pirani gauge: measures rough vacuum up to 0.4 mTorr
    - [INFICON PSG500](https://www.inficon.com/en/products/vacuum-gauge-and-controller/psg50x-psg51x)
  - Ion gauge: measure deep vacuum up to 0.0000008 mTorr
    - [Edwards AIGX-S](https://www.idealvac.com/en-us/Edwards-AIGX-S-Active-ION-Gauge-NW25-KF25-10-2-to-10-10-Torr/pp/P107159?srsltid=AfmBOooh7RYUk0mnEudKmBwehVt009TX-aN-QQBIE8pq7O3lhaWI1Nxi)

### Gas System

A small amount of argon is added to the chamber to create plasma.
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


## This project is to control different parts of the sputtering system.
Instead of having a single board computer control all the instruments, we will have a smaller board for each instrument.  
The modules will each have a Raspberry Pi Pico and communicate with each other via CAN bus.  
The modules snap together via a 6-pin magnetic pogo connector.  
 

# Building
## Setup After Cloning From GitHub

Clone with submodules and initialize nested submodules:

```bash
git clone --recurse-submodules <repo-url>
cd sputtering-control-panel
git submodule update --init --recursive
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule sync --recursive
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
Ctrl A, k
```
