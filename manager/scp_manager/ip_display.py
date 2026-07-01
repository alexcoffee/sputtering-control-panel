from __future__ import annotations

import argparse
import fcntl
import logging
import os
import socket
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

LOGGER = logging.getLogger(__name__)

SIOCGIFADDR = 0x8915
DEFAULT_WIDTH = 135
DEFAULT_HEIGHT = 240
DEFAULT_SPI_DEVICE = "/dev/spidev0.0"
DEFAULT_DC_GPIO = 25
DEFAULT_RESET_GPIO = 24
DEFAULT_BACKLIGHT_GPIO = 23
DEFAULT_POLL_INTERVAL_SEC = 2.0
DEFAULT_X_OFFSET = 50
DEFAULT_Y_OFFSET = 40


def _rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | ((blue & 0xF8) >> 3)


BLACK = _rgb565(0, 0, 0)
WHITE = _rgb565(255, 255, 255)
CYAN = _rgb565(0, 220, 255)
BLUE = _rgb565(35, 80, 180)
RED = _rgb565(255, 80, 70)


GLYPHS: dict[str, tuple[str, ...]] = {
    " ": ("00000", "00000", "00000", "00000", "00000", "00000", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    "N": ("10001", "11001", "11001", "10101", "10011", "10011", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "I": ("01110", "00100", "00100", "00100", "00100", "00100", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "W": ("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
}


def _get_interface_ipv4(interface: str) -> str | None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        ifname = interface.encode("utf-8")[:15]
        request = struct.pack("256s", ifname)
        response = fcntl.ioctl(sock.fileno(), SIOCGIFADDR, request)
        return socket.inet_ntoa(response[20:24])
    except OSError:
        return None
    finally:
        sock.close()


def list_ipv4_addresses() -> dict[str, str]:
    result: dict[str, str] = {}
    for interface in sorted(os.listdir("/sys/class/net")):
        if interface == "lo":
            continue
        ip = _get_interface_ipv4(interface)
        if ip is not None:
            result[interface] = ip
    return result


def choose_display_ipv4(addresses: dict[str, str]) -> tuple[str | None, str | None]:
    for interface in ("wlan0", "eth0"):
        ip = addresses.get(interface)
        if ip:
            return interface, ip

    for interface in sorted(addresses):
        ip = addresses[interface]
        if ip and not ip.startswith("127."):
            return interface, ip

    return None, None


def format_status_lines(interface: str | None, ip_address: str | None) -> list[str]:
    if ip_address:
        return ["WIFI", ip_address]
    if interface:
        return ["WIFI", "NO IP"]
    return ["WIFI", "NO LINK"]


class DigitalOut:
    def __init__(self, gpio: int) -> None:
        self.gpio = int(gpio)
        self._mode = "unknown"
        self._value_path = Path("/sys/class/gpio") / f"gpio{self.gpio}" / "value"
        self._export()
        self._set_direction("out")

    def _export(self) -> None:
        gpio_path = Path("/sys/class/gpio") / f"gpio{self.gpio}"
        if gpio_path.exists():
            return
        Path("/sys/class/gpio/export").write_text(f"{self.gpio}\n", encoding="ascii")
        for _ in range(100):
            if gpio_path.exists():
                return
            time.sleep(0.01)
        raise RuntimeError(f"gpio{self.gpio} did not appear under /sys/class/gpio")

    def _set_direction(self, direction: str) -> None:
        direction_path = Path("/sys/class/gpio") / f"gpio{self.gpio}" / "direction"
        direction_path.write_text(direction, encoding="ascii")
        self._mode = direction

    def write(self, value: bool) -> None:
        if self._mode != "out":
            self._set_direction("out")
        self._value_path.write_text("1" if value else "0", encoding="ascii")

    def close(self) -> None:
        try:
            Path("/sys/class/gpio/unexport").write_text(f"{self.gpio}\n", encoding="ascii")
        except OSError:
            pass


class GpioController:
    def __init__(self, gpio: int, *, retries: int = 20, retry_delay_sec: float = 0.25) -> None:
        self.gpio = int(gpio)
        self._backend: object
        try:
            import RPi.GPIO as GPIO  # type: ignore[import-not-found]
        except ImportError as exc:  # pragma: no cover - hardware dependency
            raise RuntimeError("RPi.GPIO is required for the LCD display service") from exc

        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BCM)

        last_error: Exception | None = None
        for attempt in range(max(1, int(retries))):
            try:
                GPIO.setup(self.gpio, GPIO.OUT, initial=GPIO.LOW)
                self._backend = GPIO
                self._kind = "rpi_gpio"
                return
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                GPIO.cleanup(self.gpio)
                if attempt + 1 < max(1, int(retries)):
                    time.sleep(float(retry_delay_sec))

        raise RuntimeError(f"failed to claim GPIO{self.gpio} for the LCD display") from last_error

    def write(self, value: bool) -> None:
        if self._kind == "rpi_gpio":
            gpio = self._backend
            gpio.output(self.gpio, gpio.HIGH if value else gpio.LOW)  # type: ignore[attr-defined]

    def close(self) -> None:
        if self._kind == "rpi_gpio":
            gpio = self._backend
            gpio.cleanup(self.gpio)  # type: ignore[attr-defined]


class St7789Display:
    def __init__(
        self,
        spi_device: str = DEFAULT_SPI_DEVICE,
        width: int = DEFAULT_WIDTH,
        height: int = DEFAULT_HEIGHT,
        rotation: int = 0,
        x_offset: int = 0,
        y_offset: int = 0,
        dc_gpio: int | None = None,
        reset_gpio: int | None = None,
        backlight_gpio: int | None = None,
        spi_speed_hz: int = 24_000_000,
    ) -> None:
        try:
            import spidev  # type: ignore[import-not-found]
        except ImportError as exc:  # pragma: no cover - hardware dependency
            raise RuntimeError("spidev is required for the LCD display service") from exc

        if dc_gpio is None:
            raise ValueError("dc_gpio is required for ST7789 control")

        self.width = int(width)
        self.height = int(height)
        self.rotation = int(rotation) % 4
        self.x_offset = int(x_offset)
        self.y_offset = int(y_offset)
        self._spi = spidev.SpiDev()
        self._spi.open(*self._parse_spidev_device(spi_device))
        self._spi.mode = 0
        self._spi.bits_per_word = 8
        self._spi.max_speed_hz = int(spi_speed_hz)
        self._dc = GpioController(dc_gpio) if dc_gpio is not None else None
        self._reset = GpioController(reset_gpio) if reset_gpio is not None else None
        self._backlight = GpioController(backlight_gpio) if backlight_gpio is not None else None
        self._buffer = bytearray(self.width * self.height * 2)

    @staticmethod
    def _parse_spidev_device(device: str) -> tuple[int, int]:
        value = device.strip()
        if value.startswith("/dev/spidev"):
            value = value.removeprefix("/dev/spidev")
        if "." not in value:
            raise ValueError(f"unsupported SPI device path: {device}")
        bus_text, device_text = value.split(".", 1)
        return int(bus_text), int(device_text)

    def _write_command(self, command: int, data: Iterable[int] = ()) -> None:
        if self._dc is None:
            raise RuntimeError("DC GPIO is required for ST7789 control")
        self._dc.write(False)
        self._spi.writebytes2([command & 0xFF])
        data_bytes = [int(byte) & 0xFF for byte in data]
        if data_bytes:
            self._dc.write(True)
            self._spi.writebytes2(data_bytes)

    def _hardware_reset(self) -> None:
        if self._reset is None:
            return
        self._reset.write(False)
        time.sleep(0.05)
        self._reset.write(True)
        time.sleep(0.12)

    def initialize(self) -> None:
        if self._backlight is not None:
            self._backlight.write(True)
        self._hardware_reset()
        self._write_command(0x01)
        time.sleep(0.15)
        self._write_command(0x11)
        time.sleep(0.12)
        self._write_command(0x3A, (0x55,))
        self._write_command(0x36, (self._madctl_value(),))
        self._write_command(0xB2, (0x0C, 0x0C, 0x00, 0x33, 0x33))
        self._write_command(0xB7, (0x35,))
        self._write_command(0xBB, (0x19,))
        self._write_command(0xC0, (0x2C,))
        self._write_command(0xC2, (0x01,))
        self._write_command(0xC3, (0x12,))
        self._write_command(0xC4, (0x20,))
        self._write_command(0xC6, (0x0F,))
        self._write_command(0xD0, (0xA4, 0xA1))
        self._write_command(0x21)
        self._write_command(0x13)
        time.sleep(0.05)
        self._write_command(0x29)

    def _madctl_value(self) -> int:
        rotation_map = {
            0: 0x00,
            1: 0x60,
            2: 0xC0,
            3: 0xA0,
        }
        return rotation_map[self.rotation]

    def _set_window(self, x0: int, y0: int, x1: int, y1: int) -> None:
        self._write_command(0x2A, (
            ((x0 + self.x_offset) >> 8) & 0xFF,
            (x0 + self.x_offset) & 0xFF,
            ((x1 + self.x_offset) >> 8) & 0xFF,
            (x1 + self.x_offset) & 0xFF,
        ))
        self._write_command(0x2B, (
            ((y0 + self.y_offset) >> 8) & 0xFF,
            (y0 + self.y_offset) & 0xFF,
            ((y1 + self.y_offset) >> 8) & 0xFF,
            (y1 + self.y_offset) & 0xFF,
        ))
        self._write_command(0x2C)

    def clear(self, color: int = BLACK) -> None:
        high = (color >> 8) & 0xFF
        low = color & 0xFF
        self._buffer[:] = bytes([high, low]) * (self.width * self.height)

    def _set_pixel(self, x: int, y: int, color: int) -> None:
        if not (0 <= x < self.width and 0 <= y < self.height):
            return
        offset = (y * self.width + x) * 2
        self._buffer[offset] = (color >> 8) & 0xFF
        self._buffer[offset + 1] = color & 0xFF

    def fill_rect(self, x: int, y: int, width: int, height: int, color: int) -> None:
        x_end = min(self.width, x + width)
        y_end = min(self.height, y + height)
        for row in range(max(0, y), y_end):
            row_offset = (row * self.width) * 2
            for col in range(max(0, x), x_end):
                offset = row_offset + col * 2
                self._buffer[offset] = (color >> 8) & 0xFF
                self._buffer[offset + 1] = color & 0xFF

    def _glyph(self, char: str) -> tuple[str, ...]:
        return GLYPHS.get(char, GLYPHS[" "])

    def draw_text(self, text: str, x: int, y: int, color: int, scale: int = 1) -> None:
        cursor_x = x
        for char in text:
            glyph = self._glyph(char)
            for row_index, row in enumerate(glyph):
                for col_index, bit in enumerate(row):
                    if bit != "1":
                        continue
                    x0 = cursor_x + col_index * scale
                    y0 = y + row_index * scale
                    for dy in range(scale):
                        for dx in range(scale):
                            self._set_pixel(x0 + dx, y0 + dy, color)
            cursor_x += (len(glyph[0]) + 1) * scale

    def draw_centered_text(self, text: str, y: int, color: int, scale: int = 1) -> None:
        text_width = len(text) * (6 * scale)
        x = max(0, (self.width - text_width) // 2)
        self.draw_text(text, x, y, color, scale=scale)

    def flush(self) -> None:
        self._set_window(0, 0, self.width - 1, self.height - 1)
        if self._dc is None:
            raise RuntimeError("DC GPIO is required for ST7789 control")
        self._dc.write(True)
        for offset in range(0, len(self._buffer), 4096):
            chunk = self._buffer[offset : offset + 4096]
            self._spi.writebytes2(chunk)

    def render_status(self, interface: str | None, ip_address: str | None) -> None:
        lines = format_status_lines(interface, ip_address)
        self.clear(BLACK)
        self.fill_rect(0, 0, self.width, 35, BLUE)
        self.draw_centered_text(lines[0], 8, WHITE, scale=3)

        secondary_line = lines[1]
        if ip_address:
            self.draw_centered_text(secondary_line, 114, CYAN, scale=1)
        else:
            self.draw_centered_text(secondary_line, 108, RED, scale=1)

        footer = "ONLINE" if ip_address else "WAITING"
        self.draw_centered_text(footer, 190, WHITE if ip_address else RED, scale=3)
        self.flush()

    def close(self) -> None:
        close = getattr(self._spi, "close", None)
        if callable(close):
            close()
        for controller in (self._dc, self._reset, self._backlight):
            if controller is not None:
                controller.close()


@dataclass(slots=True)
class DisplayConfig:
    spi_device: str = DEFAULT_SPI_DEVICE
    dc_gpio: int | None = None
    reset_gpio: int | None = None
    backlight_gpio: int | None = None
    width: int = DEFAULT_WIDTH
    height: int = DEFAULT_HEIGHT
    rotation: int = 0
    x_offset: int = 0
    y_offset: int = 0
    poll_interval_sec: float = DEFAULT_POLL_INTERVAL_SEC


class IpDisplayService:
    def __init__(self, config: DisplayConfig) -> None:
        self.config = config
        self.display = St7789Display(
            spi_device=config.spi_device,
            width=config.width,
            height=config.height,
            rotation=config.rotation,
            x_offset=config.x_offset,
            y_offset=config.y_offset,
            dc_gpio=config.dc_gpio,
            reset_gpio=config.reset_gpio,
            backlight_gpio=config.backlight_gpio,
        )

    def run(self) -> None:
        self.display.initialize()
        last_snapshot: tuple[str | None, str | None] | None = None
        try:
            while True:
                addresses = list_ipv4_addresses()
                interface, ip_address = choose_display_ipv4(addresses)
                snapshot = (interface, ip_address)
                if snapshot != last_snapshot:
                    LOGGER.info("display update: interface=%s ip=%s", interface, ip_address)
                    self.display.render_status(interface, ip_address)
                    last_snapshot = snapshot
                time.sleep(self.config.poll_interval_sec)
        finally:
            self.display.close()


def _env_int(name: str, default: int | None = None) -> int | None:
    value = os.getenv(name)
    if value is None or not value.strip():
        return default
    return int(value)


def _optional_int_arg(value: str) -> int | None:
    if value.strip() == "":
        return None
    return int(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Display the Pi's IP address on the ST7789 LCD")
    parser.add_argument("--spi-device", default=os.getenv("SCP_DISPLAY_SPI_DEVICE", DEFAULT_SPI_DEVICE))
    parser.add_argument("--dc-gpio", type=_optional_int_arg, default=_env_int("SCP_DISPLAY_DC_GPIO", DEFAULT_DC_GPIO))
    parser.add_argument("--reset-gpio", type=_optional_int_arg, default=_env_int("SCP_DISPLAY_RESET_GPIO", DEFAULT_RESET_GPIO))
    parser.add_argument("--backlight-gpio", type=_optional_int_arg, default=_env_int("SCP_DISPLAY_BACKLIGHT_GPIO", DEFAULT_BACKLIGHT_GPIO))
    parser.add_argument("--width", type=int, default=int(os.getenv("SCP_DISPLAY_WIDTH", str(DEFAULT_WIDTH))))
    parser.add_argument("--height", type=int, default=int(os.getenv("SCP_DISPLAY_HEIGHT", str(DEFAULT_HEIGHT))))
    parser.add_argument("--rotation", type=int, default=int(os.getenv("SCP_DISPLAY_ROTATION", "0")))
    parser.add_argument("--x-offset", type=int, default=int(os.getenv("SCP_DISPLAY_X_OFFSET", str(DEFAULT_X_OFFSET))))
    parser.add_argument("--y-offset", type=int, default=int(os.getenv("SCP_DISPLAY_Y_OFFSET", str(DEFAULT_Y_OFFSET))))
    parser.add_argument("--poll-interval-sec", type=float, default=float(os.getenv("SCP_DISPLAY_POLL_INTERVAL_SEC", str(DEFAULT_POLL_INTERVAL_SEC))))
    parser.add_argument("--log-level", default=os.getenv("SCP_DISPLAY_LOG_LEVEL", "INFO"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    config = DisplayConfig(
        spi_device=args.spi_device,
        dc_gpio=args.dc_gpio,
        reset_gpio=args.reset_gpio,
        backlight_gpio=args.backlight_gpio,
        width=args.width,
        height=args.height,
        rotation=args.rotation,
        x_offset=DEFAULT_X_OFFSET,
        y_offset=DEFAULT_Y_OFFSET,
        poll_interval_sec=args.poll_interval_sec,
    )
    IpDisplayService(config).run()


if __name__ == "__main__":
    main()
