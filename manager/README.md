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
python3 -m manager.scp_manager.app --spi-device /dev/spidev0.0
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
