from __future__ import annotations

import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


def start_broker(
    executable: str,
    listener: socket.socket,
    policy_file: Path,
) -> subprocess.Popen[bytes]:
    os.set_inheritable(listener.fileno(), True)
    return subprocess.Popen(
        [
            executable,
            "--listen-fd",
            str(listener.fileno()),
            "--policy-file",
            str(policy_file),
        ],
        pass_fds=(listener.fileno(),),
    )


def control(executable: str, socket_path: Path, *arguments: str) -> dict[str, object]:
    result = subprocess.run(
        [executable, "--socket", str(socket_path), *arguments],
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    )
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise AssertionError("controller output is not a JSON object")
    return value


def stop_broker(process: subprocess.Popen[bytes]) -> None:
    process.terminate()
    process.wait(timeout=5)


def main() -> int:
    if len(sys.argv) != 3:
        raise RuntimeError("broker and controller executable paths are required")
    broker_executable, controller_executable = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="mango-overlay-controller-") as directory:
        root = Path(directory)
        socket_path = root / "broker.sock"
        policy_file = root / "config/policy.fb"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        listener.bind(str(socket_path))
        listener.listen(4)

        broker = start_broker(broker_executable, listener, policy_file)
        try:
            initial = control(controller_executable, socket_path, "status")
            if initial.get("enabled") is not True:
                raise AssertionError("new broker did not use enabled defaults")
            disabled = control(
                controller_executable,
                socket_path,
                "set-enabled",
                "false",
            )
            if disabled.get("enabled") is not False:
                raise AssertionError("controller did not disable provider canvases")
        finally:
            stop_broker(broker)

        if stat.S_IMODE(policy_file.stat().st_mode) != 0o600:
            raise AssertionError("persisted policy permissions are not private")

        restarted = start_broker(broker_executable, listener, policy_file)
        try:
            restored = control(controller_executable, socket_path, "status")
            if restored.get("enabled") is not False:
                raise AssertionError("broker restart did not restore persisted policy")
        finally:
            stop_broker(restarted)
            listener.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
