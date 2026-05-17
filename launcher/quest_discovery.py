import socket
import time
import ipaddress


DISCOVERY_MESSAGE = b"DISCOVER_GST_QUEST_V1"
EXPECTED_PREFIX = "GST_QUEST_RECEIVER_V1"
DISCOVERY_PORT = 9010


def get_local_broadcast_addresses():
    """
    Conservative fallback list.
    For now we include the known subnet pattern plus global broadcast.
    Later the launcher can detect active adapters automatically.
    """
    return [
        "192.168.1.255",
        "255.255.255.255",
    ]


def discover_quest(timeout_seconds=5.0, interval_seconds=0.25):
    deadline = time.time() + timeout_seconds
    targets = get_local_broadcast_addresses()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(interval_seconds)

    try:
        while time.time() < deadline:
            for target in targets:
                try:
                    sock.sendto(DISCOVERY_MESSAGE, (target, DISCOVERY_PORT))
                except OSError:
                    pass

            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                continue

            text = data.decode(errors="replace").strip()

            if text.startswith(EXPECTED_PREFIX):
                parts = text.split("|")
                signaling_port = 9001

                if len(parts) >= 2:
                    try:
                        signaling_port = int(parts[1])
                    except ValueError:
                        signaling_port = 9001

                return {
                    "ip": addr[0],
                    "discovery_port": addr[1],
                    "signaling_port": signaling_port,
                    "raw": text,
                }

        return None

    finally:
        sock.close()


if __name__ == "__main__":
    result = discover_quest(timeout_seconds=5.0)

    if result is None:
        print("Quest receiver not found.")
    else:
        print("Found Quest receiver:")
        print("  IP:", result["ip"])
        print("  Signaling port:", result["signaling_port"])
        print("  Discovery reply:", result["raw"])