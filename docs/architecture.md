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
- Flash-over-CAN uses:
  - Control: `0x300 + module_id`
  - Data: `0x340 + module_id`
  - Status: `0x380 + module_id`

## RJ45 Connector
- Using standard LAN cable to connect modules to instruments/pumps.
- Each module has a RJ45 connector that has two leds: green for connection status and yellow for activity.

## Manager node
- A Raspberry Pi Zero 2 W will act as a networked manager for the CAN bus.
- It will provide a web UI, a WebSocket live stream, and command dispatch to modules.
- It will not replace the local safety behavior of Pico modules.
- The manager should be implemented as a Linux service using SocketCAN or a dedicated SPI bridge Pico.
