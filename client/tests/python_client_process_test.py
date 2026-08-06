from __future__ import annotations

import os
import binascii
import socket
import subprocess
import struct
import sys
import tempfile
import zlib


def make_png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        checksum = binascii.crc32(kind + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff"))
        + chunk(b"IEND", b"")
    )


def main() -> int:
    if len(sys.argv) != 4:
        raise RuntimeError("broker, client library, and Python source paths are required")
    broker_path, library_path, python_source = sys.argv[1:]
    sys.path.insert(0, python_source)

    from mango_overlay import ClipRect, Layout, OverlayError, Provider, Result, Visibility

    with tempfile.TemporaryDirectory(prefix="mango-overlay-python-test-") as directory:
        socket_path = os.path.join(directory, "broker.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        listener.bind(socket_path)
        listener.listen(4)
        os.set_inheritable(listener.fileno(), True)
        broker = subprocess.Popen(
            [broker_path, "--listen-fd", str(listener.fileno())],
            pass_fds=(listener.fileno(),),
        )
        listener.close()
        try:
            with Provider(
                "python-client-process-test/1.0",
                socket_path=socket_path,
                library_path=library_path,
            ) as provider:
                provider.register(
                    "dev.mango-overlay.python-test",
                    "primary",
                    "Python Client Test",
                    visibility=Visibility.ALWAYS,
                )
                png = make_png()
                provider.upload_resource(100, png)
                try:
                    provider.text(1, 10.0, 20.0, "outside transaction")
                except OverlayError as error:
                    if error.result is not Result.INVALID_STATE:
                        raise
                else:
                    raise AssertionError("text outside a transaction was accepted")

                with provider.transaction():
                    provider.group(
                        10,
                        layout=Layout(
                            translation=(40.0, 20.0),
                            opacity=0.8,
                            clip=ClipRect(0.0, 0.0, 400.0, 160.0),
                        ),
                    )
                    provider.rectangle(
                        1,
                        8.0,
                        12.0,
                        300.0,
                        80.0,
                        corner_radius=8.0,
                        color=(0.02, 0.03, 0.04, 0.9),
                        layout=Layout(parent_id=10),
                    )
                    provider.text(2, 20.0, 28.0, "Python scene", z_index=1)
                    provider.line(3, (20.0, 80.0), (260.0, 80.0), thickness=3.0)
                    provider.polyline(
                        4,
                        ((20.0, 120.0), (80.0, 100.0), (140.0, 130.0)),
                        thickness=2.0,
                    )
                    provider.circle(5, (320.0, 64.0), 24.0)
                    provider.image(6, 100, 360.0, 100.0, 48.0, 48.0)

                try:
                    with provider.transaction():
                        provider.remove(2)
                        raise ValueError("abort this transaction")
                except ValueError:
                    pass

                with provider.transaction():
                    provider.remove(2)
                    provider.remove(6)
                provider.release_resource(100)
        finally:
            broker.terminate()
            broker.wait(timeout=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
