# SCP Manager

This directory contains the Raspberry Pi Zero 2 W manager service.

Run in mock mode for local development:

```bash
python3 -m manager.scp_manager.app --can-interface mock
```

Run against a real SocketCAN interface on the Pi:

```bash
python3 -m manager.scp_manager.app --can-interface can0
```

Run against the SPI bridge Pico instead of SocketCAN:

```bash
python3 -m manager.scp_manager.app --spi-device /dev/spidev1.0
```

Install the Python SPI userspace package on the Pi Zero first:

```bash
sudo apt install python3-spidev
```

The service exposes:
- `GET /`
- `GET /api/modules`
- `GET /api/modules/{id}`
- `GET /api/modules/{id}/history`
- `GET /api/health`
- `POST /api/modules/{id}/command`
- `WS /ws`

## IP Display Service

The Pi Zero can also run a dedicated boot-time LCD service for the 1.14" ST7789V SPI display.
It reads the active IPv4 address and renders it on the screen.

Install the display dependency on Raspberry Pi OS:

```bash
sudo apt install python3-spidev
```

Then copy `manager/systemd/scp-ip-display.env.example` to `/etc/default/scp-ip-display` and set the GPIO pins if
your wiring differs from the defaults in `manager/systemd/scp-ip-display.service`.

The unit expects:
- SPI device at `/dev/spidev0.0`
- LCD `DC` on GPIO 25
- LCD `RESET` on GPIO 24
- LCD `BL` on GPIO 23

Enable it with:

```bash
sudo cp manager/systemd/scp-ip-display.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now scp-ip-display.service
```
