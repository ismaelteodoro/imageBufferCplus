#!/usr/bin/env python3

import argparse
import socket
import struct
import time
from dataclasses import dataclass
from typing import Iterable, List

import cv2
import numpy as np


FRAME_MAGIC = 0x314D5849
HEADER_STRUCT = struct.Struct("<IQQIII")
RAW10_WHITE_LEVEL = 1023.0
BAYER_TO_BGR_CODES = {
    "bg": cv2.COLOR_BayerBG2BGR,
    "gb": cv2.COLOR_BayerGB2BGR,
    "gr": cv2.COLOR_BayerGR2BGR,
    "rg": cv2.COLOR_BayerRG2BGR,
}


@dataclass(frozen=True)
class RawFrame:
    frame_id: int
    timestamp_ns: int
    width: int
    height: int
    payload: bytes


class CaptureTcpClient:
    def __init__(self, host: str, port: int, timeout_s: float = 5.0):
        self.host = host
        self.port = port
        self.timeout_s = timeout_s

    def get_latest(self) -> RawFrame:
        return self._request_single("GET_LATEST")

    def get_frame(self, frame_id: int) -> RawFrame:
        return self._request_single(f"GET_FRAME {frame_id}")

    def get_last(self, count: int) -> List[RawFrame]:
        return self._request_frames(f"GET_LAST {count}")

    def _request_single(self, command: str) -> RawFrame:
        frames = self._request_frames(command)
        if not frames:
            raise RuntimeError("server returned no frames")
        return frames[0]

    def _request_frames(self, command: str) -> List[RawFrame]:
        with socket.create_connection((self.host, self.port), timeout=self.timeout_s) as sock:
            sock.settimeout(self.timeout_s)
            sock.sendall((command + "\n").encode("ascii"))

            status = self._read_line(sock)
            if status.startswith("ERR "):
                raise RuntimeError(status)
            if not status.startswith("OK "):
                raise RuntimeError(f"invalid server response: {status!r}")

            frame_count = int(status.split()[1])
            return [self._read_frame(sock) for _ in range(frame_count)]

    def _read_frame(self, sock: socket.socket) -> RawFrame:
        header = self._read_exact(sock, HEADER_STRUCT.size)
        magic, frame_id, timestamp_ns, width, height, payload_size = HEADER_STRUCT.unpack(header)

        if magic != FRAME_MAGIC:
            raise RuntimeError(f"invalid frame magic: 0x{magic:08x}")

        payload = self._read_exact(sock, payload_size)
        return RawFrame(frame_id, timestamp_ns, width, height, payload)

    @staticmethod
    def _read_line(sock: socket.socket) -> str:
        data = bytearray()
        while True:
            chunk = sock.recv(1)
            if not chunk:
                raise RuntimeError("connection closed while reading status line")
            data += chunk
            if chunk == b"\n":
                return data.decode("ascii").strip()

    @staticmethod
    def _read_exact(sock: socket.socket, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError("connection closed while reading frame")
            data += chunk
        return bytes(data)


def unpack_raw10_csi2p(payload: bytes, width: int, height: int) -> np.ndarray:
    useful_row_bytes = width * 5 // 4
    expected_contiguous_size = useful_row_bytes * height

    payload_size = len(payload)

    if payload_size == expected_contiguous_size:
        stride_bytes = useful_row_bytes
    else:
        if payload_size % height != 0:
            raise ValueError(
                f"unexpected RAW10 payload size: got {payload_size}, "
                f"expected contiguous {expected_contiguous_size}, "
                f"and payload is not divisible by height={height}"
            )

        stride_bytes = payload_size // height

        if stride_bytes < useful_row_bytes:
            raise ValueError(
                f"invalid RAW10 stride: stride={stride_bytes}, "
                f"useful_row_bytes={useful_row_bytes}"
            )

    raw = np.frombuffer(payload, dtype=np.uint8)

    # Quebra o payload em linhas considerando o stride real enviado pelo servidor.
    rows = raw.reshape(height, stride_bytes)

    # Remove o padding do final de cada linha.
    useful = rows[:, :useful_row_bytes]

    # Agora sim o RAW10 útil pode ser interpretado em grupos de 5 bytes.
    packed = useful.reshape(-1, 5).astype(np.uint16)

    pixels = np.empty((packed.shape[0], 4), dtype=np.uint16)
    pixels[:, 0] = (packed[:, 0] << 2) | (packed[:, 4] & 0x03)
    pixels[:, 1] = (packed[:, 1] << 2) | ((packed[:, 4] >> 2) & 0x03)
    pixels[:, 2] = (packed[:, 2] << 2) | ((packed[:, 4] >> 4) & 0x03)
    pixels[:, 3] = (packed[:, 3] << 2) | ((packed[:, 4] >> 6) & 0x03)

    return pixels.reshape(height, width)


def apply_preview_curve(image: np.ndarray, args: argparse.Namespace) -> np.ndarray:
    preview = image.astype(np.float32, copy=False)

    if args.auto_levels:
        low, high = np.percentile(preview, (args.black_percentile, args.white_percentile))
        if high > low:
            preview = (preview - low) / (high - low)

    preview = np.clip(preview, 0.0, 1.0)

    if args.gamma > 0.0 and args.gamma != 1.0:
        preview = np.power(preview, 1.0 / args.gamma)

    return np.clip(preview * 255.0, 0.0, 255.0).astype(np.uint8)


def adjust_saturation_bgr(image: np.ndarray, saturation: float) -> np.ndarray:
    if saturation == 1.0:
        return image

    hsv = cv2.cvtColor(image, cv2.COLOR_BGR2HSV)
    hsv[:, :, 1] = np.clip(hsv[:, :, 1].astype(np.float32) * saturation, 0.0, 255.0).astype(np.uint8)
    return cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR)


def frame_to_display_bgr(frame: RawFrame, args: argparse.Namespace) -> np.ndarray:
    raw10 = unpack_raw10_csi2p(frame.payload, frame.width, frame.height)

    if args.no_debayer:
        gray = raw10.astype(np.float32) / RAW10_WHITE_LEVEL
        gray8 = apply_preview_curve(gray, args)
        return cv2.cvtColor(gray8, cv2.COLOR_GRAY2BGR)

    bayer_code = BAYER_TO_BGR_CODES[args.bayer]
    bgr16 = cv2.cvtColor(raw10, bayer_code)
    preview = bgr16.astype(np.float32) / RAW10_WHITE_LEVEL

    preview[:, :, 0] *= args.blue_gain
    preview[:, :, 1] *= args.green_gain
    preview[:, :, 2] *= args.red_gain

    display = apply_preview_curve(preview, args)
    return adjust_saturation_bgr(display, args.saturation)


def iter_requested_frames(client: CaptureTcpClient, args: argparse.Namespace) -> Iterable[RawFrame]:
    if args.mode == "latest":
        for _ in range(args.count):
            yield client.get_latest()
            if args.interval_ms > 0:
                time.sleep(args.interval_ms / 1000.0)
        return

    if args.mode == "last":
        yield from client.get_last(args.last)
        return

    if args.mode == "frame":
        yield client.get_frame(args.frame_id)
        return

    raise ValueError(f"unknown mode: {args.mode}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Example TCP client for imageBufferCplus RAW10 frames")
    parser.add_argument("--host", required=True, help="Raspberry Pi IP or hostname")
    parser.add_argument("--port", type=int, default=5003, help="TCP port")
    parser.add_argument("--mode", choices=("latest", "last", "frame"), default="latest")
    parser.add_argument("--count", type=int, default=20, help="Number of GET_LATEST requests")
    parser.add_argument("--last", type=int, default=5, help="Number of frames for GET_LAST")
    parser.add_argument("--frame-id", type=int, default=1, help="Frame id for GET_FRAME")
    parser.add_argument("--interval-ms", type=int, default=100, help="Delay between GET_LATEST requests")
    parser.add_argument("--no-debayer", action="store_true", help="Show raw grayscale instead of debayered BGR")
    parser.add_argument("--bayer", choices=tuple(BAYER_TO_BGR_CODES), default="rg", help="Bayer pattern used for preview")
    parser.add_argument("--red-gain", type=float, default=1.8, help="Preview gain applied to the red channel")
    parser.add_argument("--green-gain", type=float, default=1.0, help="Preview gain applied to the green channel")
    parser.add_argument("--blue-gain", type=float, default=1.8, help="Preview gain applied to the blue channel")
    parser.add_argument("--saturation", type=float, default=1.0, help="Preview color saturation multiplier")
    parser.add_argument("--gamma", type=float, default=1.0, help="Preview gamma. Use 1.0 for linear RAW display")
    parser.add_argument("--no-auto-levels", dest="auto_levels", action="store_false", help="Disable preview contrast stretching")
    parser.add_argument("--black-percentile", type=float, default=0.5, help="Low percentile for auto-levels")
    parser.add_argument("--white-percentile", type=float, default=99.5, help="High percentile for auto-levels")
    parser.set_defaults(auto_levels=True)
    parser.add_argument("--save-dir", default="", help="Optional directory to save PNG previews")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = CaptureTcpClient(args.host, args.port)

    for frame in iter_requested_frames(client, args):
        image = frame_to_display_bgr(frame, args)
        title = f"frame_id={frame.frame_id} timestamp_ns={frame.timestamp_ns}"

        cv2.imshow("imageBufferCplus TCP client", image)
        print(title)

        if args.save_dir:
            path = f"{args.save_dir.rstrip('/')}/frame_{frame.frame_id}.png"
            cv2.imwrite(path, image)

        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            break

    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
