from __future__ import annotations

import base64
import socket
import threading
import time
from typing import Optional


def base64_encode(text: str) -> str:
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def base64_decode_to_string(text: str) -> Optional[str]:
    try:
        return base64.b64decode(text.encode("ascii")).decode("utf-8", errors="replace")
    except Exception:
        return None


class SignalingClient:
    def __init__(self, running_event: threading.Event):
        self.running_event = running_event
        self.sock: Optional[socket.socket] = None

    def connect(self, host: str, port: int, attempts: int = 30, delay_s: float = 0.5) -> bool:
        print(f"[signaling] Connecting to Unity receiver at {host}:{port}...")

        last_error: Optional[BaseException] = None

        for attempt in range(1, attempts + 1):
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect((host, port))
                print("[signaling] Connected to Unity receiver.")
                return True
            except OSError as exc:
                last_error = exc
                self.close()

                print(f"[signaling] Connection attempt {attempt} failed. Retrying...")
                time.sleep(delay_s)

        print("[signaling] Could not connect to Unity receiver.")
        if last_error is not None:
            print(f"[signaling] Last socket error: {last_error}")

        return False

    def send_line(self, line: str) -> bool:
        if self.sock is None:
            print("[signaling] Cannot send: socket is invalid.")
            return False

        try:
            self.sock.sendall((line + "\n").encode("utf-8"))
            return True
        except OSError:
            print("[signaling] Failed to send data.")
            return False

    def recv_line(self) -> Optional[str]:
        if self.sock is None:
            return None

        chunks: list[bytes] = []

        while self.running_event.is_set():
            try:
                ch = self.sock.recv(1)
            except OSError:
                return None

            if not ch:
                return None

            if ch == b"\n":
                return b"".join(chunks).decode("utf-8", errors="replace")

            if ch != b"\r":
                chunks.append(ch)

        return None

    def shutdown_to_unblock(self) -> None:
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
