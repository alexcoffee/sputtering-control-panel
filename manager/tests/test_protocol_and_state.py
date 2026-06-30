from __future__ import annotations

import struct
import unittest

from manager.scp_manager.app import format_can_frame
from manager.scp_manager.protocol import (
    CanFrame,
    DISPLAY_UNIT_TORR,
    COMMAND_SET_SWITCH,
    MSG_HEARTBEAT_BASE,
    command_can_id,
    build_set_display_unit_frame,
    build_set_switch_frame,
    decode_can_frame,
)
from manager.scp_manager.state import StateStore


class ProtocolAndStateTests(unittest.TestCase):
    def test_decode_heartbeat(self) -> None:
        frame = CanFrame(
            can_id=MSG_HEARTBEAT_BASE + 2,
            data=bytes([1, 2, 2, 7]) + (1234).to_bytes(4, "little"),
        )
        decoded = decode_can_frame(frame)
        self.assertIsNotNone(decoded)
        self.assertEqual(decoded["type"], "heartbeat")
        self.assertEqual(decoded["module_id"], 2)
        self.assertEqual(decoded["heartbeat_counter"], 7)

    def test_state_store_tracks_pressure_history(self) -> None:
        store = StateStore()
        store.ingest_decoded_message(
            {
                "type": "heartbeat",
                "module_id": 2,
                "state_code": 2,
                "heartbeat_counter": 1,
                "uptime_ms": 100,
            },
            arrival_ms=1000,
        )
        store.ingest_decoded_message(
            {
                "type": "event",
                "module_id": 2,
                "event_name": "pressure_reading",
                "value": 1.5e-6,
                "unit": "torr",
                "connection_ok": True,
            },
            arrival_ms=1010,
        )
        snapshot = store.snapshot(2)
        self.assertTrue(snapshot["online"])
        self.assertEqual(snapshot["state"], "run")
        history = store.history(2)
        self.assertEqual(len(history), 1)
        self.assertEqual(history[0]["unit"], "torr")

    def test_command_validation_and_frame_build(self) -> None:
        frame = build_set_display_unit_frame(14, 2, "torr")
        self.assertEqual(frame.can_id, command_can_id(2))
        self.assertEqual(frame.data[0], 1)
        self.assertEqual(frame.data[3], DISPLAY_UNIT_TORR)

    def test_set_switch_frame_build(self) -> None:
        frame = build_set_switch_frame(14, 1, True)
        self.assertEqual(frame.can_id, command_can_id(1))
        self.assertEqual(frame.data[0], 1)
        self.assertEqual(frame.data[2], COMMAND_SET_SWITCH)
        self.assertEqual(frame.data[3], 1)

    def test_state_store_tracks_switch_state(self) -> None:
        store = StateStore()
        store.ingest_decoded_message(
            {
                "type": "event",
                "module_id": 1,
                "event_name": "switch_changed",
                "value": True,
            },
            arrival_ms=1000,
        )
        snapshot = store.snapshot(1)
        self.assertTrue(snapshot["last_switch_value"])

    def test_format_can_frame_for_logging(self) -> None:
        frame = CanFrame(can_id=0x182, data=bytes([1, 2, 4, 1, 0xAA, 0xBB]))
        self.assertEqual(format_can_frame(frame), "id=0x182 dlc=6 data=01 02 04 01 AA BB")


if __name__ == "__main__":
    unittest.main()
