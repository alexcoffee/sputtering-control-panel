from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Any
import time

from .protocol import MODULE_CATALOG, module_kind, module_name

HEARTBEAT_TIMEOUT_MS = 2000
PRESSURE_HISTORY_LIMIT = 600
COMMAND_LOG_LIMIT = 200


def now_ms() -> int:
    return int(time.time() * 1000)


@dataclass(slots=True)
class ModuleState:
    module_id: int
    module_name: str
    module_kind: str
    supported_commands: list[str] = field(default_factory=list)
    online: bool = False
    state: str = "unknown"
    last_heartbeat_ms: int | None = None
    last_seen_ms: int | None = None
    heartbeat_counter: int = 0
    uptime_ms: int = 0
    last_event: str | None = None
    last_pressure_value: float | None = None
    last_pressure_unit: str | None = None
    last_pressure_connection_ok: bool | None = None
    last_switch_value: bool | None = None
    last_power_value: float | None = None
    last_power_unit: str | None = None
    last_command_request_id: int | None = None
    last_command_status: str | None = None
    last_command_name: str | None = None
    last_command_error: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "module_id": self.module_id,
            "name": self.module_name,
            "kind": self.module_kind,
            "supported_commands": list(self.supported_commands),
            "online": self.online,
            "state": self.state,
            "last_heartbeat_ms": self.last_heartbeat_ms,
            "last_seen_ms": self.last_seen_ms,
            "heartbeat_counter": self.heartbeat_counter,
            "uptime_ms": self.uptime_ms,
            "last_event": self.last_event,
            "last_pressure_value": self.last_pressure_value,
            "last_pressure_unit": self.last_pressure_unit,
            "last_pressure_connection_ok": self.last_pressure_connection_ok,
            "last_switch_value": self.last_switch_value,
            "last_power_value": self.last_power_value,
            "last_power_unit": self.last_power_unit,
            "last_command_request_id": self.last_command_request_id,
            "last_command_status": self.last_command_status,
            "last_command_name": self.last_command_name,
            "last_command_error": self.last_command_error,
        }


@dataclass(slots=True)
class CommandRecord:
    request_id: int
    module_id: int
    command_name: str
    status: str
    error: str | None
    timestamp_ms: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "request_id": self.request_id,
            "module_id": self.module_id,
            "command": self.command_name,
            "status": self.status,
            "error": self.error,
            "timestamp_ms": self.timestamp_ms,
        }


class StateStore:
    def __init__(self) -> None:
        self._modules: dict[int, ModuleState] = {}
        self._pressure_history: dict[int, deque[dict[str, Any]]] = {}
        self._command_log: deque[CommandRecord] = deque(maxlen=COMMAND_LOG_LIMIT)
        self._last_offline_check_ms = now_ms()

    def ensure_module(self, module_id: int) -> ModuleState:
        module_id = int(module_id)
        if module_id not in self._modules:
            self._modules[module_id] = ModuleState(
                module_id=module_id,
                module_name=module_name(module_id),
                module_kind=module_kind(module_id),
                supported_commands=list(MODULE_CATALOG.get(module_id, {}).get("commands", [])),
            )
        return self._modules[module_id]

    def module(self, module_id: int) -> ModuleState | None:
        return self._modules.get(int(module_id))

    def all_modules(self) -> list[dict[str, Any]]:
        return [self._modules[module_id].to_dict() for module_id in sorted(self._modules)]

    def snapshot(self, module_id: int) -> dict[str, Any] | None:
        module = self._modules.get(int(module_id))
        return module.to_dict() if module else None

    def history(self, module_id: int) -> list[dict[str, Any]]:
        return list(self._pressure_history.get(int(module_id), ()))

    def command_log(self) -> list[dict[str, Any]]:
        return [record.to_dict() for record in self._command_log]

    def health(self) -> dict[str, Any]:
        online = sum(1 for module in self._modules.values() if module.online)
        return {
            "service": "scp-manager",
            "module_count": len(self._modules),
            "online_count": online,
            "offline_count": len(self._modules) - online,
            "last_offline_check_ms": self._last_offline_check_ms,
        }

    def ingest_decoded_message(self, message: dict[str, Any], arrival_ms: int | None = None) -> list[dict[str, Any]]:
        arrival_ms = now_ms() if arrival_ms is None else int(arrival_ms)
        events: list[dict[str, Any]] = []
        module = self.ensure_module(message["module_id"])
        module.last_seen_ms = arrival_ms

        if message["type"] == "heartbeat":
            module.online = True
            module.state = {
                0: "init",
                1: "ready",
                2: "run",
                3: "fault",
                4: "safe",
            }.get(int(message.get("state_code", -1)), "unknown")
            module.last_heartbeat_ms = arrival_ms
            module.heartbeat_counter = int(message.get("heartbeat_counter", module.heartbeat_counter))
            module.uptime_ms = int(message.get("uptime_ms", module.uptime_ms))
            events.append({"type": "module_state", "module": module.to_dict()})
            return events

        if message["type"] == "event":
            event_name = message.get("event_name", "unknown")
            module.last_event = event_name
            if event_name == "connection_detected":
                module.online = True
            elif event_name == "connection_lost":
                module.online = False
            elif event_name == "pressure_reading":
                module.last_pressure_value = float(message["value"])
                module.last_pressure_unit = str(message.get("unit", "torr"))
                module.last_pressure_connection_ok = bool(message.get("connection_ok", False))
                history = self._pressure_history.setdefault(module.module_id, deque(maxlen=PRESSURE_HISTORY_LIMIT))
                sample = {
                    "type": "pressure_sample",
                    "module_id": module.module_id,
                    "module_name": module.module_name,
                    "t_ms": arrival_ms,
                    "value": module.last_pressure_value,
                    "unit": module.last_pressure_unit,
                    "connection_ok": module.last_pressure_connection_ok,
                }
                history.append(sample)
                events.append(sample)
            elif event_name == "switch_changed":
                module.last_switch_value = bool(message.get("value", False))
            elif event_name == "power_reading":
                module.last_power_value = float(message["value"])
                module.last_power_unit = str(message.get("unit", "watts"))
                events.append(
                    {
                        "type": "power_sample",
                        "module_id": module.module_id,
                        "module_name": module.module_name,
                        "t_ms": arrival_ms,
                        "value": module.last_power_value,
                        "unit": module.last_power_unit,
                        "connection_ok": bool(message.get("connection_ok", False)),
                    }
                )
            events.append({"type": "module_state", "module": module.to_dict()})
            return events

        return events

    def mark_command_result(
        self,
        request_id: int,
        module_id: int,
        command_name: str,
        status: str,
        error: str | None = None,
    ) -> dict[str, Any]:
        timestamp_ms = now_ms()
        module = self.ensure_module(module_id)
        module.last_command_request_id = int(request_id)
        module.last_command_name = command_name
        module.last_command_status = status
        module.last_command_error = error
        record = CommandRecord(
            request_id=int(request_id),
            module_id=int(module_id),
            command_name=command_name,
            status=status,
            error=error,
            timestamp_ms=timestamp_ms,
        )
        self._command_log.append(record)
        return {
            "type": "command_result",
            "request_id": int(request_id),
            "target_module_id": int(module_id),
            "command": command_name,
            "status": status,
            "error": error,
            "timestamp_ms": timestamp_ms,
        }

    def refresh_offline_modules(self, timeout_ms: int = HEARTBEAT_TIMEOUT_MS) -> list[dict[str, Any]]:
        timestamp_ms = now_ms()
        self._last_offline_check_ms = timestamp_ms
        events: list[dict[str, Any]] = []
        for module in self._modules.values():
            if module.last_heartbeat_ms is None:
                continue
            if module.online and timestamp_ms - module.last_heartbeat_ms > timeout_ms:
                module.online = False
                module.state = "offline"
                module.last_event = "heartbeat_timeout"
                events.append({"type": "module_state", "module": module.to_dict()})
        return events

    def validate_command(self, module_id: int, command_name: str) -> tuple[bool, str | None]:
        module = self.ensure_module(module_id)
        if command_name not in module.supported_commands and command_name != "raw":
            return False, f"module {module.module_id} does not support {command_name}"
        return True, None

    def recent_command_log(self) -> list[dict[str, Any]]:
        return [record.to_dict() for record in self._command_log]
