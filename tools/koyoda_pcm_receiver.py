#!/usr/bin/env python3
import socket
import select
import wave
from pathlib import Path
from datetime import datetime
import time
import os

from faster_whisper import WhisperModel

HOST = "0.0.0.0"
PORT = 7777

SAMPLE_RATE = 22050
CHANNELS = 1
SAMPLE_WIDTH = 2

# Ignore tiny false fragments such as 0.01–0.06 s.
MIN_UTTERANCE_SEC = 0.30

# If END is lost, close the utterance after network silence.
UTTERANCE_IDLE_TIMEOUT_SEC = 2.0

# Multilingual model suitable for Thai/English.
# Override in PowerShell if desired:
#   $env:KOYODA_STT_MODEL="base"
MODEL_NAME = os.environ.get("KOYODA_STT_MODEL", "small")

print(f"Loading faster-whisper model: {MODEL_NAME}")
print("First run may download the model once.")
model = WhisperModel(
    MODEL_NAME,
    device="cpu",
    compute_type="int8",
)
print("STT model ready.")
print()


def recv_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("client disconnected")
        data.extend(chunk)
    return bytes(data)


def transcribe(path):
    try:
        segments, info = model.transcribe(
            str(path),
            beam_size=5,
            vad_filter=False,
        )

        text = " ".join(
            segment.text.strip()
            for segment in segments
            if segment.text.strip()
        ).strip()

        language = getattr(info, "language", None)
        probability = getattr(info, "language_probability", None)

        if text:
            if language and probability is not None:
                print(
                    f"STT [{language} {probability:.2f}]: {text}"
                )
            elif language:
                print(f"STT [{language}]: {text}")
            else:
                print(f"STT: {text}")
        else:
            print("STT: (no speech recognized)")

    except Exception as exc:
        print(f"STT ERROR: {exc}")


def finish_utterance(utterance, reason):
    if not utterance:
        return

    seconds = len(utterance) / (SAMPLE_RATE * SAMPLE_WIDTH)

    if seconds < MIN_UTTERANCE_SEC:
        print(
            f"IGNORED tiny fragment ({reason}): "
            f"{seconds:.2f}s / {len(utterance)} bytes"
        )
        return

    ts = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
    out = Path(f"koyoda-{ts}.wav")

    with wave.open(str(out), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(utterance)

    print(
        f"VOICE END ({reason}): "
        f"{seconds:.2f}s / {len(utterance)} bytes"
    )
    print(f"Saved: {out.resolve()}")

    transcribe(out)
    print()


print(f"KOYODA STT receiver listening on {HOST}:{PORT}")
print("ESP32 firmware is unchanged.")
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
                    readable, _, _ = select.select(
                        [conn], [], [], 0.25
                    )

                    if not readable:
                        if (
                            started
                            and last_packet_time is not None
                            and (
                                time.monotonic()
                                - last_packet_time
                            )
                            >= UTTERANCE_IDLE_TIMEOUT_SEC
                        ):
                            finish_utterance(
                                utterance,
                                "timeout fallback",
                            )
                            utterance.clear()
                            started = False
                            last_packet_time = None
                        continue

                    header = recv_exact(conn, 8)

                    if header[:4] != b"KOYA":
                        raise ValueError("bad packet magic")

                    packet_type = header[4]
                    payload_len = int.from_bytes(
                        header[5:8],
                        "big",
                    )
                    payload = (
                        recv_exact(conn, payload_len)
                        if payload_len
                        else b""
                    )

                    last_packet_time = time.monotonic()

                    if packet_type == 1:  # START
                        if started:
                            finish_utterance(
                                utterance,
                                "next START fallback",
                            )

                        utterance.clear()
                        started = True
                        print("VOICE START")

                    elif packet_type == 2 and started:  # PCM
                        utterance.extend(payload)

                    elif packet_type == 3 and started:  # END
                        finish_utterance(
                            utterance,
                            "END marker",
                        )
                        utterance.clear()
                        started = False
                        last_packet_time = None

        except (ConnectionError, OSError, ValueError) as exc:
            if started and utterance:
                finish_utterance(
                    utterance,
                    "disconnect fallback",
                )

            print("Connection closed:", exc)
            print()
