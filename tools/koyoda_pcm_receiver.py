#!/usr/bin/env python3
import socket
import select
import wave
from pathlib import Path
from datetime import datetime
import time

HOST = "0.0.0.0"
PORT = 7777

SAMPLE_RATE = 22050
CHANNELS = 1
SAMPLE_WIDTH = 2

# If an END marker is lost, finish the current utterance after this much
# network silence. This changes only the PC receiver, not KOYODA firmware.
UTTERANCE_IDLE_TIMEOUT_SEC = 2.0


def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("client disconnected")
        data.extend(chunk)
    return bytes(data)


def save_utterance(utterance, reason):
    if not utterance:
        return

    seconds = len(utterance) / (SAMPLE_RATE * SAMPLE_WIDTH)
    ts = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
    out = Path(f"koyoda-{ts}.wav")

    with wave.open(str(out), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(utterance)

    print(f"VOICE END ({reason}): {seconds:.2f}s / {len(utterance)} bytes")
    print(f"Saved: {out.resolve()}")
    print()


print(f"KOYODA receiver v2 listening on {HOST}:{PORT}")
print("Receiver-only boundary recovery enabled.")
print("KOYODA firmware remains unchanged.")
print()

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(1)

    while True:
        print("Waiting for KOYODA...")
        conn, addr = srv.accept()
        print("Connected:", addr)

        utterance = bytearray()
        started = False
        last_packet_time = None

        try:
            with conn:
                while True:
                    readable, _, _ = select.select([conn], [], [], 0.25)

                    if not readable:
                        if (
                            started
                            and last_packet_time is not None
                            and (time.monotonic() - last_packet_time)
                            >= UTTERANCE_IDLE_TIMEOUT_SEC
                        ):
                            save_utterance(utterance, "timeout fallback")
                            utterance.clear()
                            started = False
                            last_packet_time = None
                        continue

                    header = recv_exact(conn, 8)

                    if header[:4] != b"KOYA":
                        raise ValueError("bad packet magic")

                    packet_type = header[4]
                    payload_len = int.from_bytes(header[5:8], "big")
                    payload = recv_exact(conn, payload_len) if payload_len else b""
                    last_packet_time = time.monotonic()

                    if packet_type == 1:  # START
                        if started:
                            # Missing END from previous utterance:
                            # treat this new START as an implicit END.
                            save_utterance(utterance, "next START fallback")

                        utterance.clear()
                        started = True
                        print("VOICE START")

                    elif packet_type == 2 and started:  # PCM
                        utterance.extend(payload)

                    elif packet_type == 3 and started:  # END
                        save_utterance(utterance, "END marker")
                        utterance.clear()
                        started = False
                        last_packet_time = None

        except (ConnectionError, OSError, ValueError) as e:
            if started and utterance:
                save_utterance(utterance, "disconnect fallback")

            print("Connection closed:", e)
            print()
