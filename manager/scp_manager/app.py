from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .can import MockCanBus, SocketCanBus, SpiCanBus
from .protocol import (
    CanFrame,
    build_raw_command_frame,
    build_set_display_unit_frame,
    build_set_switch_frame,
    decode_can_frame,
)
from .server import ManagerServer
from .state import StateStore

MANAGER_MODULE_ID = 14
LOGGER = logging.getLogger(__name__)


@dataclass(slots=True)
class CommandRequest:
    request_id: int
    target_module_id: int
    command: str
    payload: dict[str, Any]


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    text = str(value).strip().lower()
    if text in ("1", "true", "on", "yes", "enabled"):
        return True
    if text in ("0", "false", "off", "no", "disabled"):
        return False
    raise ValueError(f"unsupported boolean value: {value}")


def format_can_frame(frame: CanFrame) -> str:
    can_id = int(frame.can_id) & 0x1FFFFFFF
    data = bytes(frame.data[:8])
    payload = " ".join(f"{byte:02X}" for byte in data) if data else "-"
    return f"id=0x{can_id:03X} dlc={len(data)} data={payload}"


class ManagerRuntime:
    def __init__(self, bus: Any, state: StateStore, source_module_id: int = MANAGER_MODULE_ID) -> None:
        self.bus = bus
        self.state = state
        self.source_module_id = source_module_id
        self._broadcast = None

    def set_broadcaster(self, broadcaster: Any) -> None:
        self._broadcast = broadcaster

    async def broadcast(self, payload: dict[str, Any]) -> None:
        if self._broadcast is not None:
            await self._broadcast(payload)

    async def handle_can_frame(self, frame: CanFrame) -> None:
        LOGGER.info("CAN RX %s", format_can_frame(frame))
        decoded = decode_can_frame(frame)
        if not decoded:
            return
        events = self.state.ingest_decoded_message(decoded)
        for event in events:
            await self.broadcast(event)

    def _build_frame(self, request: CommandRequest) -> CanFrame:
        if request.command == "set_display_unit":
            unit = request.payload.get("unit", request.payload.get("value"))
            return build_set_display_unit_frame(self.source_module_id, request.target_module_id, unit)
        if request.command == "set_switch":
            enabled = request.payload.get("enabled", request.payload.get("value"))
            return build_set_switch_frame(self.source_module_id, request.target_module_id, parse_bool(enabled))
        if request.command == "raw":
            command_id = int(request.payload["command_id"])
            payload = request.payload.get("bytes", [])
            return build_raw_command_frame(self.source_module_id, request.target_module_id, command_id, payload)
        raise ValueError(f"unsupported command: {request.command}")

    async def submit_command(self, message: dict[str, Any]) -> dict[str, Any]:
        request = CommandRequest(
            request_id=int(message.get("request_id", 0)),
            target_module_id=int(message["target_module_id"]),
            command=str(message["command"]),
            payload=dict(message.get("payload") or {}),
        )

        module = self.state.ensure_module(request.target_module_id)
        if not module.online:
            result = self.state.mark_command_result(
                request.request_id,
                request.target_module_id,
                request.command,
                "rejected",
                "module offline",
            )
            return result

        ok, error = self.state.validate_command(request.target_module_id, request.command)
        if not ok:
            result = self.state.mark_command_result(request.request_id, request.target_module_id, request.command, "rejected", error)
            return result

        try:
            frame = self._build_frame(request)
            await self.bus.send(frame)
        except Exception as exc:  # noqa: BLE001
            result = self.state.mark_command_result(
                request.request_id,
                request.target_module_id,
                request.command,
                "transport_error",
                str(exc),
            )
            return result

        result = self.state.mark_command_result(request.request_id, request.target_module_id, request.command, "accepted", None)
        return result


async def maintenance_loop(runtime: ManagerRuntime) -> None:
    while True:
        events = runtime.state.refresh_offline_modules()
        for event in events:
            await runtime.broadcast(event)
        await asyncio.sleep(1.0)


async def run_server(args: argparse.Namespace) -> None:
    if args.spi_device:
        try:
            bus = SpiCanBus(args.spi_device)
        except (OSError, RuntimeError, ValueError) as exc:
            raise SystemExit(f"failed to open SPI device {args.spi_device}: {exc}") from exc
    elif args.can_interface == "mock":
        bus = MockCanBus()
    else:
        try:
            bus = SocketCanBus(args.can_interface)
        except OSError as exc:
            raise SystemExit(f"failed to open CAN interface {args.can_interface}: {exc}") from exc

    state = StateStore()
    runtime = ManagerRuntime(bus=bus, state=state)
    server = ManagerServer(runtime=runtime, static_dir=Path(__file__).resolve().parents[1] / "static")
    runtime.set_broadcaster(server.broadcast)

    server_task = await asyncio.start_server(server.handle_connection, args.host, args.port)
    can_task = asyncio.create_task(bus.receive_loop(runtime.handle_can_frame))
    maintenance_task = asyncio.create_task(maintenance_loop(runtime))

    try:
        async with server_task:
            try:
                await server_task.serve_forever()
            except asyncio.CancelledError:
                pass
    finally:
        can_task.cancel()
        maintenance_task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await can_task
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await maintenance_task
        close = getattr(bus, "close", None)
        if callable(close):
            close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SCP manager service")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--can-interface", default="mock", help="Linux CAN interface name or 'mock'")
    parser.add_argument("--spi-device", default=None, help="SPI device path such as /dev/spidev0.0")
    parser.add_argument("--log-level", default="INFO", help="Python logging level")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    asyncio.run(run_server(args))


if __name__ == "__main__":
    main()
