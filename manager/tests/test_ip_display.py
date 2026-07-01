from __future__ import annotations

import unittest
from builtins import __import__ as builtins_import
from unittest.mock import Mock, patch

from manager.scp_manager.ip_display import DEFAULT_X_OFFSET, DEFAULT_Y_OFFSET, GpioController, St7789Display, choose_display_ipv4, format_status_lines, parse_args


class IpDisplayTests(unittest.TestCase):
    def test_choose_display_ipv4_prefers_wifi(self) -> None:
        interface, ip_address = choose_display_ipv4({"eth0": "192.168.1.20", "wlan0": "192.168.1.42"})
        self.assertEqual(interface, "wlan0")
        self.assertEqual(ip_address, "192.168.1.42")

    def test_choose_display_ipv4_falls_back_to_any_interface(self) -> None:
        interface, ip_address = choose_display_ipv4({"enp1s0": "10.0.0.5"})
        self.assertEqual(interface, "enp1s0")
        self.assertEqual(ip_address, "10.0.0.5")

    def test_format_status_lines(self) -> None:
        self.assertEqual(format_status_lines("wlan0", "192.168.1.42"), ["WIFI", "192.168.1.42"])
        self.assertEqual(format_status_lines(None, None), ["WIFI", "NO LINK"])

    def test_e_glyph_is_defined(self) -> None:
        display = object.__new__(St7789Display)
        self.assertEqual(display._glyph("E"), ("11111", "10000", "10000", "11110", "10000", "10000", "11111"))

    def test_flush_sets_dc_high_before_pixel_data(self) -> None:
        display = object.__new__(St7789Display)
        events: list[tuple[str, object]] = []

        class DcStub:
            def write(self, value: bool) -> None:
                events.append(("dc", value))

        class SpiStub:
            def writebytes2(self, data) -> None:  # type: ignore[no-untyped-def]
                events.append(("spi", bytes(data)))

        display.width = 2
        display.height = 1
        display._buffer = bytearray([1, 2, 3, 4])
        display._dc = DcStub()
        display._spi = SpiStub()
        display._set_window = Mock()

        display.flush()

        self.assertEqual(events[0], ("dc", True))
        self.assertEqual(events[1], ("spi", b"\x01\x02\x03\x04"))
        display._set_window.assert_called_once_with(0, 0, 1, 0)

    def test_parse_args_uses_display_x_offset_default(self) -> None:
        with patch("sys.argv", ["ip_display"]):
            args = parse_args()
        self.assertEqual(args.x_offset, DEFAULT_X_OFFSET)

    def test_parse_args_uses_display_y_offset_default(self) -> None:
        with patch("sys.argv", ["ip_display"]):
            args = parse_args()
        self.assertEqual(args.y_offset, DEFAULT_Y_OFFSET)

    def test_gpio_controller_requires_rpi_gpio(self) -> None:
        def fake_import(name, globals=None, locals=None, fromlist=(), level=0):  # type: ignore[no-untyped-def]
            if name == "RPi.GPIO":
                raise ImportError("missing")
            return builtins_import(name, globals, locals, fromlist, level)

        with patch("builtins.__import__", side_effect=fake_import):
            with self.assertRaises(RuntimeError):
                GpioController(25)


if __name__ == "__main__":
    unittest.main()
