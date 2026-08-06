from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 6:
        raise RuntimeError(
            "cargo, broker, client library, manifest, and target directory are required"
        )
    cargo, broker_path, library_path, manifest, target_directory = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="mango-overlay-rust-test-") as directory:
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
            library_directory = os.path.dirname(library_path)
            environment["MANGO_OVERLAY_CLIENT_LIB_DIR"] = library_directory
            environment["MANGO_OVERLAY_TEST_SOCKET"] = socket_path
            environment["CARGO_TARGET_DIR"] = target_directory
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(
                part
                for part in (
                    library_directory,
                    environment.get("LD_LIBRARY_PATH", ""),
                )
                if part
            )
            subprocess.run(
                [
                    cargo,
                    "test",
                    "--quiet",
                    "--manifest-path",
                    manifest,
                    "--test",
                    "provider_process",
                ],
                check=True,
                env=environment,
            )
        finally:
            broker.terminate()
            broker.wait(timeout=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
