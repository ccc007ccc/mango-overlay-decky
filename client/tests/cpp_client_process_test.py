from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        raise RuntimeError("broker, client test, and client library paths are required")
    broker_path, test_path, library_path = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="mango-overlay-cpp-test-") as directory:
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
            environment = os.environ.copy()
            environment["MANGO_OVERLAY_TEST_SOCKET"] = socket_path
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(
                part
                for part in (
                    os.path.dirname(library_path),
                    environment.get("LD_LIBRARY_PATH", ""),
                )
                if part
            )
            subprocess.run([test_path], check=True, env=environment)
        finally:
            broker.terminate()
            broker.wait(timeout=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
