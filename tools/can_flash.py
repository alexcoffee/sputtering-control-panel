#!/usr/bin/env python3
"""Flash a module over CAN through the usb_can_bridge firmware."""

from __future__ import annotations

import argparse
import importlib
import pathlib
import struct
import sys
import time
import zlib


USB_CAN_PACKET_MAGIC = b"\xA5\x5A"
USB_CAN_PACKET_SIZE = 16

USB_CAN_PACKET_TYPE_CAN_TX = 1
USB_CAN_PACKET_TYPE_CAN_RX = 129

SCP_PROTOCOL_VERSION = 1
SCP_MSG_FLASH_CONTROL_BASE = 0x300
SCP_MSG_FLASH_DATA_BASE = 0x340
SCP_MSG_FLASH_STATUS_BASE = 0x380

SCP_FLASH_COMMAND_BEGIN = 1
SCP_FLASH_COMMAND_FINISH = 2
SCP_FLASH_COMMAND_COMMIT = 3
SCP_FLASH_COMMAND_ABORT = 4
SCP_FLASH_COMMAND_REBOOT_TO_BOOTLOADER = 6

SCP_FLASH_STATUS_ACK = 1
SCP_FLASH_STATUS_PROGRESS = 2
SCP_FLASH_STATUS_READY = 3
SCP_FLASH_STATUS_COMMITTING = 4
SCP_FLASH_STATUS_ERROR = 127

SCP_FLASH_ERROR_INVALID_ARGUMENT = 2
SCP_FLASH_ERROR_BAD_SEQUENCE = 4
SCP_FLASH_ERROR_IMAGE_INCOMPLETE = 7
SCP_FLASH_PROGRESS_INTERVAL_FRAMES = 16
SCP_FLASH_DATA_BYTES_PER_FRAME = 6
SCP_FLASH_FRAME_DELAY_US = 300
SCP_FLASH_PROGRESS_BAR_WIDTH = 32
SCP_FLASH_MIN_FRAME_DELAY_US = 100
SCP_FLASH_MAX_FRAME_DELAY_US = 2000
SCP_FLASH_DELAY_STEP_UP_US = 80
SCP_FLASH_DELAY_STEP_DOWN_US = 20
SCP_FLASH_STABLE_WINDOWS_FOR_SPEEDUP = 6
SCP_FLASH_MAX_PROGRESS_TIMEOUTS = 12


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flash firmware over CAN using usb_can_bridge.")
    parser.add_argument("--port", required=True, help="USB serial port for the bridge (for example /dev/ttyACM0).")
    parser.add_argument("--bin", required=True, help="Path to firmware .bin image.")
    parser.add_argument("--target-id", required=True, type=int, help="Target CAN module ID.")
    parser.add_argument("--source-id", type=int, default=12, help="Source module ID used in flash control frames.")
    parser.add_argument(
        "--session-id",
        type=int,
        default=None,
        help="Flash session ID (0-255). If omitted, one is auto-generated per run.",
    )
    parser.add_argument("--baud", type=int, default=2000000, help="Serial baud rate for CDC ACM.")
    parser.add_argument("--timeout", type=float, default=1.0, help="Read timeout in seconds.")
    parser.add_argument(
        "--bootloader-reboot",
        dest="bootloader_reboot",
        action="store_true",
        default=True,
        help="Ask target firmware to reboot into tiny CAN bootloader before flashing (default: enabled).",
    )
    parser.add_argument(
        "--no-bootloader-reboot",
        dest="bootloader_reboot",
        action="store_false",
        help="Do not request reboot into tiny CAN bootloader before flashing.",
    )
    parser.add_argument(
        "--frame-delay-us",
        type=int,
        default=SCP_FLASH_FRAME_DELAY_US,
        help="Delay between CAN data frames in microseconds (default: 300).",
    )
    parser.add_argument(
        "--adaptive-pacing",
        dest="adaptive_pacing",
        action="store_true",
        default=True,
        help="Automatically adjust frame delay based on transfer stability (default: enabled).",
    )
    parser.add_argument(
        "--no-adaptive-pacing",
        dest="adaptive_pacing",
        action="store_false",
        help="Disable automatic frame delay adjustment.",
    )
    parser.add_argument(
        "--min-frame-delay-us",
        type=int,
        default=SCP_FLASH_MIN_FRAME_DELAY_US,
        help="Minimum delay used by adaptive pacing in microseconds.",
    )
    parser.add_argument(
        "--max-frame-delay-us",
        type=int,
        default=SCP_FLASH_MAX_FRAME_DELAY_US,
        help="Maximum delay used by adaptive pacing in microseconds.",
    )
    return parser.parse_args()


def resolve_pyserial():
    try:
        serial_module = importlib.import_module("serial")
    except Exception as exc:
        raise RuntimeError(
            "Failed to import pyserial module 'serial'. Install it with: pip install pyserial"
        ) from exc

    serial_cls = getattr(serial_module, "Serial", None)
    if serial_cls is None:
        module_path = getattr(serial_module, "__file__", "<unknown>")
        raise RuntimeError(
            "Imported module 'serial' does not provide Serial class "
            f"(loaded from: {module_path}). "
            "This usually means the wrong package is installed. "
            "Run: pip uninstall serial ; pip install pyserial"
        )

    return serial_module, serial_cls


def build_usb_can_tx_frame(can_id: int, dlc: int, data: bytes) -> bytes:
    payload = bytearray(USB_CAN_PACKET_SIZE)
    payload[0:2] = USB_CAN_PACKET_MAGIC
    payload[2] = USB_CAN_PACKET_TYPE_CAN_TX
    payload[4:6] = struct.pack("<H", can_id & 0x7FF)
    payload[6] = dlc & 0x0F
    payload[8:16] = data.ljust(8, b"\x00")[:8]
    return bytes(payload)


def iter_usb_packets(ser, sync: bytearray, deadline: float):
    while time.monotonic() < deadline:
        waiting = getattr(ser, "in_waiting", 0)
        read_len = waiting if waiting > 0 else 1
        chunk = ser.read(read_len)
        if not chunk:
            time.sleep(0.001)
            continue
        sync.extend(chunk)
        while True:
            marker = sync.find(USB_CAN_PACKET_MAGIC)
            if marker < 0:
                if len(sync) > 2:
                    del sync[:-2]
                break
            if marker > 0:
                del sync[:marker]
            if len(sync) < USB_CAN_PACKET_SIZE:
                break
            pkt = bytes(sync[:USB_CAN_PACKET_SIZE])
            del sync[:USB_CAN_PACKET_SIZE]
            yield pkt


def parse_can_rx_packet(packet: bytes):
    if len(packet) != USB_CAN_PACKET_SIZE:
        return None
    if packet[:2] != USB_CAN_PACKET_MAGIC or packet[2] != USB_CAN_PACKET_TYPE_CAN_RX:
        return None
    can_id = struct.unpack_from("<H", packet, 4)[0] & 0x7FF
    dlc = packet[6] & 0x0F
    data = packet[8:16]
    return can_id, dlc, data


def send_flash_control(ser, source_id: int, target_id: int, command: int, session_id: int, argument: int):
    can_id = SCP_MSG_FLASH_CONTROL_BASE + target_id
    frame = bytes(
        [
            SCP_PROTOCOL_VERSION,
            source_id & 0xFF,
            command & 0xFF,
            session_id & 0xFF,
        ]
    ) + struct.pack("<I", argument & 0xFFFFFFFF)
    ser.write(build_usb_can_tx_frame(can_id, 8, frame))


def send_flash_data(ser, target_id: int, session_id: int, sequence: int, payload: bytes):
    can_id = SCP_MSG_FLASH_DATA_BASE + target_id
    frame = bytes([session_id & 0xFF, sequence & 0xFF]) + payload.ljust(SCP_FLASH_DATA_BYTES_PER_FRAME, b"\xFF")
    ser.write(build_usb_can_tx_frame(can_id, 8, frame))


def render_progress(done: int, total: int, resync_count: int, elapsed_s: float, frame_delay_us: int):
    total_safe = max(total, 1)
    done_clamped = min(max(done, 0), total_safe)
    ratio = done_clamped / total_safe
    filled = int(ratio * SCP_FLASH_PROGRESS_BAR_WIDTH)
    bar = "#" * filled + "-" * (SCP_FLASH_PROGRESS_BAR_WIDTH - filled)
    suffix = f"  resync:{resync_count}" if resync_count > 0 else ""
    sys.stdout.write(
        f"\r[{bar}] {ratio * 100.0:6.2f}% ({done_clamped}/{total})"
        f"  elapsed:{elapsed_s:6.1f}s  delay:{frame_delay_us:4d}us{suffix}"
    )
    sys.stdout.flush()


def wait_for_flash_status(
    ser,
    sync: bytearray,
    target_id: int,
    session_id: int,
    expected_statuses: set[int],
    timeout_s: float,
    min_progress: int = 0,
):
    status_id = SCP_MSG_FLASH_STATUS_BASE + target_id
    deadline = time.monotonic() + timeout_s
    latest_progress = None

    for packet in iter_usb_packets(ser, sync, deadline):
        decoded = parse_can_rx_packet(packet)
        if decoded is None:
            continue
        can_id, dlc, data = decoded
        if can_id != status_id or dlc < 8:
            continue
        if data[0] != SCP_PROTOCOL_VERSION or data[1] != (target_id & 0xFF):
            continue
        status = data[2]
        rx_session_id = data[3]
        arg = struct.unpack_from("<I", data, 4)[0]
        if rx_session_id != (session_id & 0xFF):
            continue

        if status == SCP_FLASH_STATUS_PROGRESS:
            if arg >= min_progress and (latest_progress is None or arg > latest_progress):
                latest_progress = arg
        if status in expected_statuses:
            if status == SCP_FLASH_STATUS_PROGRESS and arg < min_progress:
                continue
            return status, arg, latest_progress

    raise TimeoutError(f"Timed out waiting for flash status {sorted(expected_statuses)}")


def abort_flash_session(ser, sync: bytearray, source_id: int, target_id: int, session_id: int):
    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_ABORT, session_id, 0)
    try:
        wait_for_flash_status(
            ser,
            sync,
            target_id,
            session_id,
            {SCP_FLASH_STATUS_ACK, SCP_FLASH_STATUS_ERROR},
            timeout_s=1.0,
        )
    except TimeoutError:
        pass


def try_reboot_into_bootloader(ser, sync: bytearray, source_id: int, target_id: int, session_id: int) -> bool:
    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_REBOOT_TO_BOOTLOADER, session_id, 0)
    try:
        status, arg, _ = wait_for_flash_status(
            ser,
            sync,
            target_id,
            session_id,
            {SCP_FLASH_STATUS_ACK, SCP_FLASH_STATUS_ERROR},
            timeout_s=1.5,
        )
    except TimeoutError:
        return False

    if status == SCP_FLASH_STATUS_ACK:
        print("Target accepted bootloader reboot command.")
        time.sleep(0.9)
        sync.clear()
        return True

    if arg == SCP_FLASH_ERROR_INVALID_ARGUMENT:
        return False

    print(f"Bootloader reboot command returned error code={arg}; continuing without reboot.")
    return False


def query_progress_via_finish_probe(
    ser,
    sync: bytearray,
    source_id: int,
    target_id: int,
    session_id: int,
    image_crc32: int,
    min_progress: int,
):
    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_FINISH, session_id, image_crc32)
    try:
        status, arg, latest_progress = wait_for_flash_status(
            ser,
            sync,
            target_id,
            session_id,
            {SCP_FLASH_STATUS_PROGRESS, SCP_FLASH_STATUS_ERROR},
            timeout_s=2.0,
            min_progress=min_progress,
        )
    except TimeoutError:
        return None

    if status == SCP_FLASH_STATUS_PROGRESS:
        return arg

    if arg == SCP_FLASH_ERROR_IMAGE_INCOMPLETE:
        if latest_progress is not None:
            return latest_progress
        try:
            _, progress, _ = wait_for_flash_status(
                ser,
                sync,
                target_id,
                session_id,
                {SCP_FLASH_STATUS_PROGRESS},
                timeout_s=2.0,
                min_progress=min_progress,
            )
            return progress
        except TimeoutError:
            return None

    return None


def flash_image(
    ser,
    image: bytes,
    source_id: int,
    target_id: int,
    session_id: int,
    frame_delay_us: int,
    adaptive_pacing: bool,
    min_frame_delay_us: int,
    max_frame_delay_us: int,
    bootloader_reboot: bool,
):
    total_size = len(image)
    image_crc32 = zlib.crc32(image) & 0xFFFFFFFF
    print(f"Target module: {target_id}")
    print(f"Session ID: {session_id}")
    print(f"Image size: {total_size} bytes")
    print(f"Image CRC32: 0x{image_crc32:08X}")

    packet_sync = bytearray()
    confirmed_progress = 0
    setup_session_id = (session_id + 97) & 0xFF

    abort_flash_session(ser, packet_sync, source_id, target_id, setup_session_id)
    if bootloader_reboot and try_reboot_into_bootloader(ser, packet_sync, source_id, target_id, setup_session_id):
        abort_flash_session(ser, packet_sync, source_id, target_id, setup_session_id)
    time.sleep(0.02)
    packet_sync.clear()

    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_BEGIN, session_id, total_size)
    status, arg, _ = wait_for_flash_status(
        ser, packet_sync, target_id, session_id, {SCP_FLASH_STATUS_ACK, SCP_FLASH_STATUS_ERROR}, timeout_s=20.0
    )
    if status == SCP_FLASH_STATUS_ERROR:
        raise RuntimeError(f"BEGIN rejected, error code={arg}")
    if arg != total_size:
        raise RuntimeError(f"BEGIN ack mismatch (expected size {total_size}, got {arg})")

    offset = 0
    sequence = 0
    frames_since_sync = 0
    resync_count = 0
    current_delay_us = max(0, frame_delay_us)
    stable_windows = 0
    progress_timeout_count = 0
    start_time = time.monotonic()

    def tune_delay(had_resync: bool):
        nonlocal current_delay_us
        nonlocal stable_windows

        if not adaptive_pacing:
            return

        if had_resync:
            stable_windows = 0
            current_delay_us = min(max_frame_delay_us, current_delay_us + SCP_FLASH_DELAY_STEP_UP_US)
            return

        stable_windows += 1
        if stable_windows >= SCP_FLASH_STABLE_WINDOWS_FOR_SPEEDUP:
            stable_windows = 0
            current_delay_us = max(min_frame_delay_us, current_delay_us - SCP_FLASH_DELAY_STEP_DOWN_US)

    render_progress(0, total_size, resync_count, 0.0, current_delay_us)
    try:
        while offset < total_size:
            frame_delay_s = current_delay_us / 1_000_000.0
            chunk = image[offset : offset + SCP_FLASH_DATA_BYTES_PER_FRAME]
            send_flash_data(ser, target_id, session_id, sequence, chunk)
            if frame_delay_s > 0:
                time.sleep(frame_delay_s)
            offset += len(chunk)
            sequence = (sequence + 1) & 0xFF
            frames_since_sync += 1

            if frames_since_sync >= SCP_FLASH_PROGRESS_INTERVAL_FRAMES or offset >= total_size:
                try:
                    status, arg, latest_progress = wait_for_flash_status(
                        ser,
                        packet_sync,
                        target_id,
                        session_id,
                        {SCP_FLASH_STATUS_PROGRESS, SCP_FLASH_STATUS_ERROR},
                        timeout_s=2.0,
                        min_progress=confirmed_progress,
                    )
                except TimeoutError:
                    latest_progress = query_progress_via_finish_probe(
                        ser,
                        packet_sync,
                        source_id,
                        target_id,
                        session_id,
                        image_crc32,
                        confirmed_progress,
                    )
                    if latest_progress is None:
                        progress_timeout_count += 1
                        if progress_timeout_count >= SCP_FLASH_MAX_PROGRESS_TIMEOUTS:
                            raise RuntimeError("Timed out waiting for progress status")
                        frames_since_sync = 0
                        tune_delay(True)
                        elapsed_s = time.monotonic() - start_time
                        render_progress(
                            confirmed_progress,
                            total_size,
                            resync_count,
                            elapsed_s,
                            current_delay_us,
                        )
                        continue
                    progress_timeout_count = 0
                    if latest_progress > confirmed_progress:
                        confirmed_progress = latest_progress
                    resync_count += 1
                    offset = min(confirmed_progress, total_size)
                    sequence = (offset // SCP_FLASH_DATA_BYTES_PER_FRAME) & 0xFF
                    frames_since_sync = 0
                    tune_delay(True)
                    elapsed_s = time.monotonic() - start_time
                    render_progress(
                        confirmed_progress,
                        total_size,
                        resync_count,
                        elapsed_s,
                        current_delay_us,
                    )
                    continue
                progress_timeout_count = 0
                if latest_progress is not None and latest_progress > confirmed_progress:
                    confirmed_progress = latest_progress

                if status == SCP_FLASH_STATUS_ERROR:
                    if arg == SCP_FLASH_ERROR_BAD_SEQUENCE:
                        try:
                            _, progress_arg, _ = wait_for_flash_status(
                                ser,
                                packet_sync,
                                target_id,
                                session_id,
                                {SCP_FLASH_STATUS_PROGRESS},
                                timeout_s=0.25,
                                min_progress=confirmed_progress,
                            )
                            if progress_arg > confirmed_progress:
                                confirmed_progress = progress_arg
                        except TimeoutError:
                            latest_progress = query_progress_via_finish_probe(
                                ser,
                                packet_sync,
                                source_id,
                                target_id,
                                session_id,
                                image_crc32,
                                confirmed_progress,
                            )
                            if latest_progress is not None and latest_progress > confirmed_progress:
                                confirmed_progress = latest_progress
                        if confirmed_progress > 0:
                            resync_count += 1
                            offset = min(confirmed_progress, total_size)
                            sequence = (offset // SCP_FLASH_DATA_BYTES_PER_FRAME) & 0xFF
                            frames_since_sync = 0
                            tune_delay(True)
                            elapsed_s = time.monotonic() - start_time
                            render_progress(
                                confirmed_progress,
                                total_size,
                                resync_count,
                                elapsed_s,
                                current_delay_us,
                            )
                            time.sleep(0.005)
                            continue
                    raise RuntimeError(f"DATA rejected, error code={arg}")

                frames_since_sync = 0
                tune_delay(False)
                if arg > confirmed_progress:
                    confirmed_progress = arg
                if confirmed_progress > offset:
                    offset = confirmed_progress
                    sequence = (offset // SCP_FLASH_DATA_BYTES_PER_FRAME) & 0xFF
                elapsed_s = time.monotonic() - start_time
                render_progress(confirmed_progress, total_size, resync_count, elapsed_s, current_delay_us)
    finally:
        sys.stdout.write("\n")
        sys.stdout.flush()

    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_FINISH, session_id, image_crc32)
    status, arg, _ = wait_for_flash_status(
        ser, packet_sync, target_id, session_id, {SCP_FLASH_STATUS_READY, SCP_FLASH_STATUS_ERROR}, timeout_s=5.0
    )
    if status == SCP_FLASH_STATUS_ERROR:
        raise RuntimeError(f"FINISH rejected, error code={arg}")

    send_flash_control(ser, source_id, target_id, SCP_FLASH_COMMAND_COMMIT, session_id, 0)
    status, arg, _ = wait_for_flash_status(
        ser, packet_sync, target_id, session_id, {SCP_FLASH_STATUS_COMMITTING, SCP_FLASH_STATUS_ERROR}, timeout_s=5.0
    )
    if status == SCP_FLASH_STATUS_ERROR:
        raise RuntimeError(f"COMMIT rejected, error code={arg}")

    print(f"Commit accepted ({arg} bytes). Target is rebooting.")


def main() -> int:
    args = parse_args()
    try:
        _, serial_cls = resolve_pyserial()
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    image_path = pathlib.Path(args.bin)
    if not image_path.is_file():
        print(f"Image not found: {image_path}", file=sys.stderr)
        return 2

    image = image_path.read_bytes()
    if not image:
        print("Image is empty.", file=sys.stderr)
        return 2

    if not (0 <= args.target_id <= 255):
        print("--target-id must be between 0 and 255", file=sys.stderr)
        return 2
    if not (0 <= args.source_id <= 255):
        print("--source-id must be between 0 and 255", file=sys.stderr)
        return 2
    session_id = args.session_id
    if session_id is None:
        session_id = (time.monotonic_ns() >> 20) & 0xFF
        if session_id == 0:
            session_id = 1
    if not (0 <= session_id <= 255):
        print("--session-id must be between 0 and 255", file=sys.stderr)
        return 2
    if args.frame_delay_us < 0:
        print("--frame-delay-us must be >= 0", file=sys.stderr)
        return 2
    if args.min_frame_delay_us < 0:
        print("--min-frame-delay-us must be >= 0", file=sys.stderr)
        return 2
    if args.max_frame_delay_us < 0:
        print("--max-frame-delay-us must be >= 0", file=sys.stderr)
        return 2
    if args.min_frame_delay_us > args.max_frame_delay_us:
        print("--min-frame-delay-us must be <= --max-frame-delay-us", file=sys.stderr)
        return 2

    with serial_cls(args.port, args.baud, timeout=0, write_timeout=args.timeout) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        packet_sync = bytearray()
        try:
            flash_image(
                ser,
                image,
                args.source_id,
                args.target_id,
                session_id,
                args.frame_delay_us,
                args.adaptive_pacing,
                args.min_frame_delay_us,
                args.max_frame_delay_us,
                args.bootloader_reboot,
            )
        except Exception as exc:
            abort_flash_session(ser, packet_sync, args.source_id, args.target_id, session_id)
            print(f"Flash failed: {exc}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
