from __future__ import annotations

import asyncio
import base64
import hashlib
import contextlib
import json
from pathlib import Path
import struct
from typing import Any
from urllib.parse import urlparse


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WebSocketSession:
    def __init__(self, server: "ManagerServer", reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.server = server
        self.reader = reader
        self.writer = writer
        self.queue: asyncio.Queue[str] = asyncio.Queue(maxsize=256)
        self.alive = True

    async def send_json(self, payload: dict[str, Any]) -> None:
        if not self.alive:
            return
        text = json.dumps(payload, separators=(",", ":"), sort_keys=True)
        try:
            self.queue.put_nowait(text)
        except asyncio.QueueFull:
            try:
                _ = self.queue.get_nowait()
            except asyncio.QueueEmpty:
                pass
            self.queue.put_nowait(text)

    async def run(self) -> None:
        sender = asyncio.create_task(self._sender())
        try:
            await self.server.send_initial_snapshot(self)
            while self.alive:
                opcode, payload = await read_ws_frame(self.reader)
                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    await write_ws_frame(self.writer, 0xA, payload)
                    continue
                if opcode != 0x1:
                    continue
                message = json.loads(payload.decode("utf-8"))
                await self.server.handle_ws_message(self, message)
        finally:
            self.alive = False
            sender.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await sender
            self.writer.close()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self.writer.wait_closed()
            self.server.remove_client(self)

    async def _sender(self) -> None:
        while self.alive:
            text = await self.queue.get()
            await write_ws_frame(self.writer, 0x1, text.encode("utf-8"))


async def read_http_request(reader: asyncio.StreamReader) -> tuple[str, str, dict[str, str], bytes]:
    request_line = await reader.readline()
    if not request_line:
        raise ConnectionError("client disconnected")
    method, target, _version = request_line.decode("utf-8").rstrip("\r\n").split(" ", 2)
    headers: dict[str, str] = {}
    while True:
        line = await reader.readline()
        if not line or line in (b"\r\n", b"\n"):
            break
        key, value = line.decode("utf-8").rstrip("\r\n").split(":", 1)
        headers[key.strip().lower()] = value.strip()
    body = b""
    content_length = int(headers.get("content-length", "0"))
    if content_length > 0:
        body = await reader.readexactly(content_length)
    return method, target, headers, body


async def write_http_response(
    writer: asyncio.StreamWriter,
    status_code: int,
    status_text: str,
    body: bytes,
    content_type: str = "application/json",
    extra_headers: dict[str, str] | None = None,
) -> None:
    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(body)),
        "Connection": "close",
    }
    if extra_headers:
        headers.update(extra_headers)
    response = [f"HTTP/1.1 {status_code} {status_text}\r\n"]
    for key, value in headers.items():
        response.append(f"{key}: {value}\r\n")
    response.append("\r\n")
    writer.write("".join(response).encode("utf-8") + body)
    await writer.drain()


async def write_ws_handshake(writer: asyncio.StreamWriter, sec_websocket_key: str) -> None:
    accept = base64.b64encode(hashlib.sha1((sec_websocket_key + WS_GUID).encode("utf-8")).digest()).decode("ascii")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "\r\n"
    )
    writer.write(response.encode("utf-8"))
    await writer.drain()


async def write_ws_frame(writer: asyncio.StreamWriter, opcode: int, payload: bytes) -> None:
    payload = bytes(payload)
    header = bytearray()
    header.append(0x80 | (opcode & 0x0F))
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < (1 << 16):
        header.append(126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(127)
        header.extend(struct.pack("!Q", length))
    writer.write(bytes(header) + payload)
    await writer.drain()


async def read_ws_frame(reader: asyncio.StreamReader) -> tuple[int, bytes]:
    first = await reader.readexactly(2)
    opcode = first[0] & 0x0F
    masked = bool(first[1] & 0x80)
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", await reader.readexactly(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if masked else b"\x00\x00\x00\x00"
    payload = await reader.readexactly(length) if length else b""
    if masked:
        payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return opcode, payload


class ManagerServer:
    def __init__(self, runtime: Any, static_dir: Path) -> None:
        self.runtime = runtime
        self.static_dir = static_dir
        self._clients: set[WebSocketSession] = set()

    def add_client(self, session: WebSocketSession) -> None:
        self._clients.add(session)

    def remove_client(self, session: WebSocketSession) -> None:
        self._clients.discard(session)

    async def broadcast(self, payload: dict[str, Any]) -> None:
        if not self._clients:
            return
        stale: list[WebSocketSession] = []
        for session in list(self._clients):
            if not session.alive:
                stale.append(session)
                continue
            await session.send_json(payload)
        for session in stale:
            self.remove_client(session)

    async def send_initial_snapshot(self, session: WebSocketSession) -> None:
        await session.send_json({"type": "module_list", "modules": self.runtime.state.all_modules(), "health": self.runtime.state.health()})
        for module in self.runtime.state.all_modules():
            history = self.runtime.state.history(module["module_id"])
            if history:
                await session.send_json({"type": "history", "module_id": module["module_id"], "samples": history})

    async def handle_ws_message(self, session: WebSocketSession, message: dict[str, Any]) -> None:
        message_type = message.get("type")
        if message_type == "subscribe":
            modules = message.get("modules", [])
            if modules == "all" or modules == []:
                modules = [module["module_id"] for module in self.runtime.state.all_modules()]
            await session.send_json({"type": "module_list", "modules": self.runtime.state.all_modules(), "health": self.runtime.state.health()})
            for module_id in modules:
                history = self.runtime.state.history(int(module_id))
                if history:
                    await session.send_json({"type": "history", "module_id": int(module_id), "samples": history})
            return

        if message_type == "command":
            result = await self.runtime.submit_command(message)
            await self.broadcast(result)
            return

        await session.send_json({"type": "error", "error": f"unsupported websocket message type: {message_type}"})

    async def handle_http(self, method: str, target: str, headers: dict[str, str], body: bytes, writer: asyncio.StreamWriter) -> bool:
        parsed = urlparse(target)
        path = parsed.path
        if method in ("GET", "HEAD") and path == "/":
            index_path = self.static_dir / "index.html"
            body = index_path.read_bytes()
            await write_http_response(writer, 200, "OK", body, content_type="text/html; charset=utf-8")
            return True

        if method in ("GET", "HEAD") and path == "/api/modules":
            body = json.dumps({"modules": self.runtime.state.all_modules(), "health": self.runtime.state.health()}, separators=(",", ":")).encode("utf-8")
            await write_http_response(writer, 200, "OK", body)
            return True

        if method in ("GET", "HEAD") and path.startswith("/api/modules/") and path.endswith("/history"):
            parts = path.split("/")
            module_id = int(parts[3])
            body = json.dumps({"module_id": module_id, "samples": self.runtime.state.history(module_id)}, separators=(",", ":")).encode("utf-8")
            await write_http_response(writer, 200, "OK", body)
            return True

        if method in ("GET", "HEAD") and path.startswith("/api/modules/"):
            module_id = int(path.rsplit("/", 1)[-1])
            snapshot = self.runtime.state.snapshot(module_id)
            if snapshot is None:
                await write_http_response(writer, 404, "Not Found", b'{"error":"module not found"}')
            else:
                body = json.dumps(snapshot, separators=(",", ":")).encode("utf-8")
                await write_http_response(writer, 200, "OK", body)
            return True

        if method in ("GET", "HEAD") and path == "/api/health":
            body = json.dumps(self.runtime.state.health(), separators=(",", ":")).encode("utf-8")
            await write_http_response(writer, 200, "OK", body)
            return True

        if method == "POST" and path.startswith("/api/modules/") and path.endswith("/command"):
            module_id = int(path.split("/")[3])
            payload = json.loads(body.decode("utf-8") or "{}")
            payload["target_module_id"] = module_id
            result = await self.runtime.submit_command(payload)
            await write_http_response(writer, 200, "OK", json.dumps(result, separators=(",", ":")).encode("utf-8"))
            await self.broadcast(result)
            return True

        return False

    async def handle_connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            method, target, headers, body = await read_http_request(reader)
            if method == "GET" and target == "/ws" and headers.get("upgrade", "").lower() == "websocket":
                key = headers.get("sec-websocket-key")
                if not key:
                    await write_http_response(writer, 400, "Bad Request", b'{"error":"missing websocket key"}')
                    return
                await write_ws_handshake(writer, key)
                session = WebSocketSession(self, reader, writer)
                self.add_client(session)
                await session.run()
                return
            handled = await self.handle_http(method, target, headers, body, writer)
            if not handled:
                await write_http_response(writer, 404, "Not Found", b'{"error":"not found"}')
        except Exception as exc:  # noqa: BLE001
            if not writer.is_closing():
                payload = json.dumps({"error": str(exc)}, separators=(",", ":")).encode("utf-8")
                try:
                    await write_http_response(writer, 500, "Internal Server Error", payload)
                except Exception:  # noqa: BLE001
                    pass
        finally:
            if not writer.is_closing():
                writer.close()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await writer.wait_closed()
