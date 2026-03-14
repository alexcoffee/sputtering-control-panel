# Sputter Control Panel Firmware Layout

## Structure
- `bsp/pico/`: RP2040-specific hardware boundary (CAN, IRQ, clocks, GPIO wrappers).
- `shared/`: Protocol and module runtime used by every firmware image.
- `modules/<module_name>/`: One firmware app per control-panel module.
- `can2040/`, `pico-sdk/`: Third-party submodules.

## Build model
- Build one module image by setting `SCP_MODULE_TARGET`.
- Optional: build all module images with `-DSCP_BUILD_ALL_MODULES=ON`.

Example:
```bash
./build.sh ui_panel
./build.sh interlocks
cmake -S . -B build/all -DSCP_BUILD_ALL_MODULES=ON
cmake --build build/all
```

## CAN conventions
- Heartbeats use IDs `0x100 + module_id`.
- Lower IDs are reserved for high-priority safety/fault traffic.
- Keep all message IDs and wire formats in `shared/include/scp/protocol.h`.

## RJ45 Connector
- Using standard LAN cable to connect modules to instruments/pumps.
- Each module has a RJ45 connector that has two leds: green for connection status and yellow for activity.
