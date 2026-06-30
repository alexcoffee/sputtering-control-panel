from __future__ import annotations

from dataclasses import dataclass
from typing import Any
import struct

PROTOCOL_VERSION = 1
CAN_BITRATE = 500_000

MSG_HEARTBEAT_BASE = 0x100
MSG_EVENT_BASE = 0x180
MSG_COMMAND_BASE = 0x200

EVENT_SWITCH_CHANGED = 1
EVENT_CONNECTION_DETECTED = 2
EVENT_CONNECTION_LOST = 3
EVENT_PRESSURE_READING = 4
EVENT_POWER_READING = 5

COMMAND_SET_DISPLAY_UNIT = 1
COMMAND_SET_SWITCH = 2

DISPLAY_UNIT_TORR = 0
DISPLAY_UNIT_BAR = 1
DISPLAY_UNIT_VOLTAGE = 2

MODULE_CATALOG = {
    1: {"name": "Roughing Pump", "kind": "control", "telemetry": "switch", "commands": ["set_switch"]},
    2: {"name": "Pirani Gauge", "kind": "pressure", "telemetry": "pressure", "commands": ["set_display_unit"]},
    3: {"name": "Ion Gauge", "kind": "pressure", "telemetry": "pressure", "commands": ["set_display_unit"]},
    4: {"name": "Turbo Pump", "kind": "power", "telemetry": "power", "commands": ["set_display_unit"]},
    11: {"name": "Monitor", "kind": "ui", "telemetry": "state", "commands": []},
    12: {"name": "USB-CAN Bridge", "kind": "bridge", "telemetry": "state", "commands": []},
    13: {"name": "Pirani Sim", "kind": "pressure", "telemetry": "pressure", "commands": ["set_display_unit"]},
    14: {"name": "Manager", "kind": "manager", "telemetry": "state", "commands": []},
    15: {"name": "SPI-CAN Bridge", "kind": "bridge", "telemetry": "state", "commands": []},
}

DISPLAY_UNIT_NAMES = {
    DISPLAY_UNIT_TORR: "torr",
    DISPLAY_UNIT_BAR: "bar",
    DISPLAY_UNIT_VOLTAGE: "voltage",
}

DISPLAY_UNIT_VALUES = {v: k for k, v in DISPLAY_UNIT_NAMES.items()}


@dataclass(slots=True)
class CanFrame:
    can_id: int
    data: bytes


def heartbeat_can_id(module_id: int) -> int:
    return MSG_HEARTBEAT_BASE + int(module_id)


def event_can_id(module_id: int) -> int:
    return MSG_EVENT_BASE + int(module_id)


def command_can_id(module_id: int) -> int:
    return MSG_COMMAND_BASE + int(module_id)


def module_name(module_id: int) -> str:
    entry = MODULE_CATALOG.get(int(module_id))
    return entry["name"] if entry else f"Module {module_id}"


def module_kind(module_id: int) -> str:
    entry = MODULE_CATALOG.get(int(module_id))
    return entry["kind"] if entry else "unknown"


def supported_commands(module_id: int) -> list[str]:
    entry = MODULE_CATALOG.get(int(module_id))
    return list(entry["commands"]) if entry else []


def display_unit_to_value(unit: str | int) -> int:
    if isinstance(unit, int):
        if unit in DISPLAY_UNIT_NAMES:
            return unit
        raise ValueError(f"unsupported display unit value: {unit}")
    key = str(unit).strip().lower()
    if key not in DISPLAY_UNIT_VALUES:
        raise ValueError(f"unsupported display unit: {unit}")
    return DISPLAY_UNIT_VALUES[key]


def display_unit_name(value: int) -> str:
    return DISPLAY_UNIT_NAMES.get(int(value), "unknown")


def pack_can_frame(frame: CanFrame) -> bytes:
    if frame is None:
        raise ValueError("frame is required")
    payload = bytes(frame.data[:8]).ljust(8, b"\x00")
    dlc = min(len(frame.data), 8)
    return struct.pack("=IB3x8s", int(frame.can_id), dlc, payload)


def unpack_can_frame(raw: bytes) -> CanFrame:
    can_id, dlc, payload = struct.unpack("=IB3x8s", raw[:16])
    return CanFrame(can_id=can_id, data=bytes(payload[:dlc]))


def build_set_display_unit_frame(source_module_id: int, target_module_id: int, unit: str | int) -> CanFrame:
    unit_value = display_unit_to_value(unit)
    data = bytes(
        [
            PROTOCOL_VERSION,
            int(source_module_id) & 0xFF,
            COMMAND_SET_DISPLAY_UNIT,
            unit_value & 0xFF,
            0,
            0,
            0,
            0,
        ]
    )
    return CanFrame(can_id=command_can_id(target_module_id), data=data)


def build_set_switch_frame(source_module_id: int, target_module_id: int, enabled: bool) -> CanFrame:
    data = bytes(
        [
            PROTOCOL_VERSION,
            int(source_module_id) & 0xFF,
            COMMAND_SET_SWITCH,
            1 if enabled else 0,
            0,
            0,
            0,
            0,
        ]
    )
    return CanFrame(can_id=command_can_id(target_module_id), data=data)


def build_raw_command_frame(
    source_module_id: int,
    target_module_id: int,
    command_id: int,
    payload: list[int] | bytes | bytearray | None = None,
) -> CanFrame:
    data = bytearray(8)
    data[0] = PROTOCOL_VERSION
    data[1] = int(source_module_id) & 0xFF
    data[2] = int(command_id) & 0xFF
    if payload:
        payload_bytes = bytes(payload)[:5]
        data[3 : 3 + len(payload_bytes)] = payload_bytes
    return CanFrame(can_id=command_can_id(target_module_id), data=bytes(data))


def decode_can_frame(frame: CanFrame) -> dict[str, Any] | None:
    can_id = int(frame.can_id) & 0x7FF
    data = bytes(frame.data)
    if 0 <= can_id - MSG_HEARTBEAT_BASE < 0x80 and len(data) >= 8:
        module_id = can_id - MSG_HEARTBEAT_BASE
        uptime_ms = int.from_bytes(data[4:8], "little", signed=False)
        return {
            "type": "heartbeat",
            "module_id": module_id,
            "protocol_version": data[0],
            "state_code": data[2],
            "heartbeat_counter": data[3],
            "uptime_ms": uptime_ms,
        }
    if 0 <= can_id - MSG_EVENT_BASE < 0x80 and len(data) >= 4:
        module_id = can_id - MSG_EVENT_BASE
        event_type = data[2]
        payload = data[3:8]
        decoded: dict[str, Any] = {
            "type": "event",
            "module_id": module_id,
            "protocol_version": data[0] if len(data) > 0 else None,
            "event_type": event_type,
            "event_name": {
                EVENT_SWITCH_CHANGED: "switch_changed",
                EVENT_CONNECTION_DETECTED: "connection_detected",
                EVENT_CONNECTION_LOST: "connection_lost",
                EVENT_PRESSURE_READING: "pressure_reading",
                EVENT_POWER_READING: "power_reading",
            }.get(event_type, "unknown"),
        }
        if event_type in (EVENT_PRESSURE_READING, EVENT_POWER_READING) and len(data) >= 8:
            value = struct.unpack("<f", data[4:8])[0]
            decoded["value"] = value
            decoded["connection_ok"] = bool(data[3])
            decoded["unit"] = "torr" if event_type == EVENT_PRESSURE_READING else "watts"
        elif event_type == EVENT_SWITCH_CHANGED:
            decoded["value"] = bool(data[3])
        elif event_type in (EVENT_CONNECTION_DETECTED, EVENT_CONNECTION_LOST):
            decoded["value"] = bool(data[3])
        else:
            decoded["payload"] = list(payload)
        return decoded
    return None
