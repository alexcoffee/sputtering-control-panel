from __future__ import annotations

from dataclasses import dataclass
import asyncio
import math
import re
import socket
import struct
import time
from typing import Awaitable, Callable

from .protocol import (
    CanFrame,
    EVENT_POWER_READING,
    EVENT_PRESSURE_READING,
    MSG_EVENT_BASE,
    MSG_HEARTBEAT_BASE,
    pack_can_frame,
    unpack_can_frame,
)

OnFrame = Callable[[CanFrame], Awaitable[None] | None]


class SocketCanBus:
    def __init__(self, interface_name: str) -> None:
        self._interface_name = interface_name
        self._socket = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self._socket.setblocking(False)
        self._socket.bind((interface_name,))

    @property
    def interface_name(self) -> str:
        return self._interface_name

    async def send(self, frame: CanFrame) -> None:
        await asyncio.to_thread(self._socket.send, pack_can_frame(frame))

    async def receive_loop(self, on_frame: OnFrame) -> None:
        loop = asyncio.get_running_loop()
        while True:
            raw = await loop.sock_recv(self._socket, 16)
            if not raw:
                await asyncio.sleep(0.01)
                continue
            frame = unpack_can_frame(raw)
            result = on_frame(frame)
            if asyncio.iscoroutine(result):
                await result

    def close(self) -> None:
        self._socket.close()


SPI_CAN_PACKET_MAGIC_0 = 0xA5
SPI_CAN_PACKET_MAGIC_1 = 0x5A
SPI_CAN_PACKET_SIZE = 16
SPI_CAN_PACKET_TYPE_IDLE = 0
SPI_CAN_PACKET_TYPE_CAN_TX = 1
SPI_CAN_PACKET_TYPE_PING = 2
SPI_CAN_PACKET_TYPE_SET_BITRATE = 3
SPI_CAN_PACKET_TYPE_STATUS = 128
SPI_CAN_PACKET_TYPE_CAN_RX = 129
SPI_CAN_PACKET_TYPE_PONG = 130

SPI_CAN_STATUS_OK = 0


def _parse_spidev_device(device: str) -> tuple[int, int]:
    value = device.strip()
    match = re.fullmatch(r"(?:/dev/)?spidev(\d+)\.(\d+)", value)
    if match:
        return int(match.group(1)), int(match.group(2))

    match = re.fullmatch(r"(\d+)\.(\d+)", value)
    if match:
        return int(match.group(1)), int(match.group(2))

    raise ValueError(f"unsupported SPI device path: {device}")


def _build_spi_idle_packet() -> bytes:
    return bytes(SPI_CAN_PACKET_SIZE)


def _build_spi_can_tx_packet(frame: CanFrame) -> bytes:
    payload = bytes(frame.data[:8])
    packet = bytearray(SPI_CAN_PACKET_SIZE)
    packet[0] = SPI_CAN_PACKET_MAGIC_0
    packet[1] = SPI_CAN_PACKET_MAGIC_1
    packet[2] = SPI_CAN_PACKET_TYPE_CAN_TX
    packet[4] = int(frame.can_id) & 0xFF
    packet[5] = (int(frame.can_id) >> 8) & 0xFF
    packet[6] = len(payload) & 0x0F
    packet[8 : 8 + len(payload)] = payload
    return bytes(packet)


def _decode_spi_frame(packet: bytes) -> CanFrame | None:
    if len(packet) < SPI_CAN_PACKET_SIZE:
        return None
    if packet[0] != SPI_CAN_PACKET_MAGIC_0 or packet[1] != SPI_CAN_PACKET_MAGIC_1:
        return None
    if packet[2] != SPI_CAN_PACKET_TYPE_CAN_RX:
        return None

    can_id = int(packet[4]) | (int(packet[5]) << 8)
    dlc = min(int(packet[6]) & 0x0F, 8)
    return CanFrame(can_id=can_id, data=bytes(packet[8 : 8 + dlc]))


def _decode_spi_status(packet: bytes) -> tuple[int, int] | None:
    if len(packet) < SPI_CAN_PACKET_SIZE:
        return None
    if packet[0] != SPI_CAN_PACKET_MAGIC_0 or packet[1] != SPI_CAN_PACKET_MAGIC_1:
        return None
    if packet[2] != SPI_CAN_PACKET_TYPE_STATUS:
        return None
    argument = int.from_bytes(packet[8:12], "little", signed=False)
    return int(packet[3]), argument


class SpiCanBus:
    def __init__(self, device_path: str, max_speed_hz: int = 1_000_000) -> None:
        try:
            import spidev  # type: ignore[import-not-found]
        except ImportError as exc:  # pragma: no cover - hardware dependency
            raise RuntimeError("spidev is required for SPI CAN transport") from exc

        bus, device = _parse_spidev_device(device_path)
        self._device_path = device_path
        self._spi = spidev.SpiDev()
        self._spi.open(bus, device)
        self._spi.mode = 0
        self._spi.bits_per_word = 8
        self._spi.max_speed_hz = int(max_speed_hz)
        self._lock = asyncio.Lock()
        self._rx_queue: asyncio.Queue[CanFrame] = asyncio.Queue(maxsize=256)
        self._packet_buffer = bytearray()
        self._status_queue: asyncio.Queue[tuple[int, int]] = asyncio.Queue(maxsize=16)

    @property
    def device_path(self) -> str:
        return self._device_path

    async def _transfer(self, packet: bytes) -> bytes:
        def transfer_bytes() -> bytes:
            response = bytearray()
            for byte in packet:
                response.append(int(self._spi.xfer2([int(byte) & 0xFF])[0]) & 0xFF)
            return bytes(response)

        return await asyncio.to_thread(transfer_bytes)

    async def _dispatch_spi_packet(self, packet: bytes, on_frame: OnFrame | None, check_status: bool) -> None:
        frame = _decode_spi_frame(packet)
        if frame is not None:
            if on_frame is None:
                try:
                    self._rx_queue.put_nowait(frame)
                except asyncio.QueueFull:
                    _ = self._rx_queue.get_nowait()
                    self._rx_queue.put_nowait(frame)
            else:
                result = on_frame(frame)
                if asyncio.iscoroutine(result):
                    await result
            return

        status = _decode_spi_status(packet)
        if status is None:
            return

        if not check_status:
            try:
                self._status_queue.put_nowait(status)
            except asyncio.QueueFull:
                _ = self._status_queue.get_nowait()
                self._status_queue.put_nowait(status)
            return

        status_code, argument = status
        if status_code != SPI_CAN_STATUS_OK:
            raise RuntimeError(f"spi bridge returned status {status_code} (argument={argument})")

    async def _handle_response(self, data: bytes, on_frame: OnFrame | None, check_status: bool) -> None:
        if not data:
            return

        self._packet_buffer.extend(data)
        while len(self._packet_buffer) >= 2:
            try:
                magic_index = self._packet_buffer.index(SPI_CAN_PACKET_MAGIC_0)
            except ValueError:
                self._packet_buffer.clear()
                return

            if magic_index > 0:
                del self._packet_buffer[:magic_index]
            if len(self._packet_buffer) < 2:
                return
            if self._packet_buffer[1] != SPI_CAN_PACKET_MAGIC_1:
                del self._packet_buffer[0]
                continue
            if len(self._packet_buffer) < SPI_CAN_PACKET_SIZE:
                return

            packet = bytes(self._packet_buffer[:SPI_CAN_PACKET_SIZE])
            del self._packet_buffer[:SPI_CAN_PACKET_SIZE]
            await self._dispatch_spi_packet(packet, on_frame, check_status)

    async def send(self, frame: CanFrame) -> None:
        request = _build_spi_can_tx_packet(frame)
        idle = _build_spi_idle_packet()
        async with self._lock:
            first_response = await self._transfer(request)
            await self._handle_response(first_response, None, check_status=False)
            for _ in range(16):
                try:
                    status_code, argument = self._status_queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
                else:
                    if status_code != SPI_CAN_STATUS_OK:
                        raise RuntimeError(f"spi bridge returned status {status_code} (argument={argument})")
                    return

                response = await self._transfer(idle)
                await self._handle_response(response, None, check_status=False)
                try:
                    status_code, argument = self._status_queue.get_nowait()
                except asyncio.QueueEmpty:
                    continue
                if status_code != SPI_CAN_STATUS_OK:
                    raise RuntimeError(f"spi bridge returned status {status_code} (argument={argument})")
                return

            raise RuntimeError("spi bridge did not return CAN_TX status")

    async def receive_loop(self, on_frame: OnFrame) -> None:
        idle = _build_spi_idle_packet()
        while True:
            while True:
                try:
                    frame = self._rx_queue.get_nowait()
                except asyncio.QueueEmpty:
                    break
                result = on_frame(frame)
                if asyncio.iscoroutine(result):
                    await result

            async with self._lock:
                response = await self._transfer(idle)
            await self._handle_response(response, on_frame, check_status=False)
            await asyncio.sleep(0.01)

    def close(self) -> None:
        self._spi.close()


@dataclass(slots=True)
class MockModuleConfig:
    module_id: int
    telemetry: str


class MockCanBus:
    def __init__(self) -> None:
        self._sent_frames: list[CanFrame] = []
        self._module_state: dict[int, dict[str, float | int | bool]] = {
            2: {"counter": 0, "phase": 0.0},
            3: {"counter": 0, "phase": 1.0},
            4: {"counter": 0, "phase": 0.5},
            13: {"counter": 0, "phase": 1.5},
        }
        self._pressure_modules = {2, 3, 13}
        self._power_modules = {4}

    @property
    def sent_frames(self) -> list[CanFrame]:
        return list(self._sent_frames)

    async def send(self, frame: CanFrame) -> None:
        self._sent_frames.append(frame)

    async def receive_loop(self, on_frame: OnFrame) -> None:
        next_heartbeat: dict[int, float] = {module_id: time.monotonic() for module_id in self._module_state}
        next_sample: dict[int, float] = {module_id: time.monotonic() for module_id in self._module_state}
        while True:
            now = time.monotonic()
            for module_id in list(self._module_state):
                if now >= next_heartbeat[module_id]:
                    counter = int(self._module_state[module_id]["counter"])
                    heartbeat = CanFrame(
                        can_id=MSG_HEARTBEAT_BASE + module_id,
                        data=bytes([1, module_id & 0xFF, 2, counter & 0xFF, 0, 0, 0, 0]),
                    )
                    result = on_frame(heartbeat)
                    if asyncio.iscoroutine(result):
                        await result
                    self._module_state[module_id]["counter"] = counter + 1
                    next_heartbeat[module_id] = now + 0.5

                if now >= next_sample[module_id]:
                    phase = float(self._module_state[module_id]["phase"])
                    if module_id in self._pressure_modules:
                        value = 10 ** (-6 + 0.25 * math.sin(now + phase))
                        sample = struct.pack("<f", float(value))
                        event = CanFrame(
                            can_id=MSG_EVENT_BASE + module_id,
                            data=bytes([1, module_id & 0xFF, EVENT_PRESSURE_READING, 1]) + sample,
                        )
                        result = on_frame(event)
                        if asyncio.iscoroutine(result):
                            await result
                    elif module_id in self._power_modules:
                        value = 120.0 + 15.0 * math.sin(now + phase)
                        sample = struct.pack("<f", float(value))
                        event = CanFrame(
                            can_id=MSG_EVENT_BASE + module_id,
                            data=bytes([1, module_id & 0xFF, EVENT_POWER_READING, 1]) + sample,
                        )
                        result = on_frame(event)
                        if asyncio.iscoroutine(result):
                            await result
                    next_sample[module_id] = now + 1.0
            await asyncio.sleep(0.05)
