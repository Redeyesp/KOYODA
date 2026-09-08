#!/usr/bin/env python3
import socket
import wave
from pathlib import Path
from datetime import datetime

HOST = "0.0.0.0"
PORT = 7777
SAMPLE_RATE = 22050
CHANNELS = 1
SAMPLE_WIDTH = 2

def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("client disconnected")
        data.extend(chunk)
    return bytes(data)

print(f"KOYODA receiver listening on {HOST}:{PORT}")

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

        try:
            with conn:
                while True:
                    header = recv_exact(conn, 8)
                    if header[:4] != b"KOYA":
                        raise ValueError("bad packet magic")

                    packet_type = header[4]
                    payload_len = int.from_bytes(header[5:8], "big")
                    payload = recv_exact(conn, payload_len) if payload_len else b""

                    if packet_type == 1:
                        utterance.clear()
                        started = True
                        print("VOICE START")

                    elif packet_type == 2 and started:
                        utterance.extend(payload)

                    elif packet_type == 3 and started:
                        seconds = len(utterance) / (SAMPLE_RATE * SAMPLE_WIDTH)
                        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
                        out = Path(f"koyoda-{ts}.wav")

                        with wave.open(str(out), "wb") as wf:
                            wf.setnchannels(CHANNELS)
                            wf.setsampwidth(SAMPLE_WIDTH)
                            wf.setframerate(SAMPLE_RATE)
                            wf.writeframes(utterance)

                        print(f"VOICE END: {seconds:.2f}s / {len(utterance)} bytes")
                        print(f"Saved: {out.resolve()}")
                        utterance.clear()
                        started = False

        except (ConnectionError, OSError, ValueError) as e:
            print("Connection closed:", e)
