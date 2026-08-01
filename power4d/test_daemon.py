#!/usr/bin/env python3

import fcntl
import hashlib
import hmac
import os
import pathlib
import pty
import socket
import subprocess
import sys
import tempfile
import threading
import time


PORT = 4244
PASSWORD = "correct horse battery staple"
PASSWORD2 = "another correct horse battery staple"
CHALLENGE = "A" * 43
REPORTS = ["batteries", "banks", "relays", "inputs", "parameters", "logs"]


def report_json(name):
    return f'{{"type":"test","report":"{name}"}}'


def p4j1(json_text, valid=True):
    digest = hashlib.sha1(json_text.encode()).hexdigest()
    if not valid:
        digest = "0" * 40
    return f"P4J1 {len(json_text.encode())} {digest} {json_text}"


def receive_until(connection, delimiter):
    data = bytearray()
    while not data.endswith(delimiter):
        chunk = connection.recv(1)
        if not chunk:
            raise RuntimeError("client closed connection")
        data.extend(chunk)
    return bytes(data)


def authenticate(connection, passwords=(PASSWORD,)):
    connection.sendall(f"authenticate {CHALLENGE}\r\n".encode())
    response = receive_until(connection, b"\n").strip().decode()
    matched = None
    for password in passwords:
        expected = hmac.new(password.encode(), CHALLENGE.encode(), hashlib.sha256).hexdigest()
        if response == f"authenticate {expected}":
            matched = password
            break
    if matched is None:
        raise AssertionError(f"unexpected authentication response: {response}")
    connection.sendall(b"authenticated\r\npower4> ")
    return matched


def tcp_server(address, ready, errors, invalid_report=None, stall=False, oversized=False):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((address, PORT))
            server.listen(1)
            ready.set()
            with server.accept()[0] as connection:
                connection.settimeout(5)
                authenticate(connection)
                if stall:
                    time.sleep(3)
                    return
                for name in REPORTS:
                    command = receive_until(connection, b"\r")[:-1].decode()
                    if command != f"p4exec report {name}":
                        raise AssertionError(f"unexpected command: {command}")
                    if oversized:
                        connection.sendall(b"x" * 131073)
                        return
                    json_text = report_json(name)
                    frame = p4j1(json_text, valid=name != invalid_report)
                    connection.sendall(
                        f"console noise\r\n..dot-stuffed noise\r\n{frame}\r\n.\r\npower4> ".encode()
                    )
    except Exception as error:
        errors.append(error)


def concurrent_tcp_server(ready, errors):
    handlers = []

    def handle(connection):
        try:
            with connection:
                connection.settimeout(5)
                password = authenticate(connection, (PASSWORD, PASSWORD2))
                if password == PASSWORD2:
                    time.sleep(3)
                    return
                for name in REPORTS:
                    command = receive_until(connection, b"\r")[:-1].decode()
                    if command != f"p4exec report {name}":
                        raise AssertionError(f"unexpected command: {command}")
                    json_text = report_json(name)
                    frame = p4j1(json_text, valid=name != "banks")
                    connection.sendall(f"{frame}\r\n.\r\npower4> ".encode())
        except Exception as error:
            errors.append(error)

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", PORT))
            server.listen(2)
            ready.set()
            for _ in range(2):
                handler = threading.Thread(target=handle, args=(server.accept()[0],))
                handler.start()
                handlers.append(handler)
            for handler in handlers:
                handler.join(5)
    except Exception as error:
        errors.append(error)


def wait_for_reports(root, node, expected, timeout=5):
    deadline = time.monotonic() + timeout
    paths = {name: pathlib.Path(root) / node / f"{name}.json" for name in expected}
    while time.monotonic() < deadline and not all(path.exists() for path in paths.values()):
        time.sleep(0.01)
    for name, path in paths.items():
        if not path.exists():
            raise AssertionError(f"missing {node}/{name}.json")
        if path.read_text() != report_json(name) + "\n":
            raise AssertionError(f"unexpected contents in {path}")


def stop(process):
    process.terminate()
    _, stderr = process.communicate(timeout=5)
    if process.returncode not in (-15, 0):
        raise AssertionError(stderr)
    return stderr


def run_tcp_case(binary):
    ready = threading.Event()
    errors = []
    server = threading.Thread(
        target=tcp_server,
        args=("127.0.0.1", ready, errors),
    )
    server.start()
    if not ready.wait(5):
        raise RuntimeError("TCP server did not start")

    with tempfile.TemporaryDirectory() as root:
        environment = os.environ.copy()
        environment["PW1"] = PASSWORD
        process = subprocess.Popen(
            [binary, "--verbose", "-o", root, "tcp:127.0.0.1:PW1:shed"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        server.join(5)
        if server.is_alive():
            stop(process)
            raise RuntimeError("TCP collection did not finish")
        if errors:
            stop(process)
            raise errors[0]
        wait_for_reports(root, "shed", REPORTS)
        stderr = stop(process)
        if PASSWORD in stderr:
            raise AssertionError("verbose logging exposed a password value")


def run_oversized_line_case(binary):
    ready = threading.Event()
    errors = []
    server = threading.Thread(
        target=tcp_server,
        args=("127.0.0.1", ready, errors),
        kwargs={"oversized": True},
    )
    server.start()
    if not ready.wait(5):
        raise RuntimeError("oversized-line TCP server did not start")

    with tempfile.TemporaryDirectory() as root:
        environment = os.environ.copy()
        environment["PW1"] = PASSWORD
        process = subprocess.Popen(
            [binary, "-o", root, "tcp:127.0.0.1:PW1:large"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        server.join(5)
        if server.is_alive():
            stop(process)
            raise RuntimeError("oversized-line TCP collection did not finish")
        if errors:
            stop(process)
            raise errors[0]
        time.sleep(0.1)
        stderr = stop(process)
        if "exceeds 131072 bytes" not in stderr:
            raise AssertionError(f"oversized line was not diagnosed: {stderr}")


def run_partial_and_concurrent_case(binary):
    ready = threading.Event()
    errors = []
    server = threading.Thread(target=concurrent_tcp_server, args=(ready, errors))
    server.start()
    if not ready.wait(5):
        raise RuntimeError("concurrent TCP servers did not start")

    with tempfile.TemporaryDirectory() as root:
        stale = pathlib.Path(root) / "good"
        stale.mkdir()
        (stale / "banks.json").write_text("old\n")
        environment = os.environ.copy()
        environment.update({"PW1": PASSWORD, "PW2": PASSWORD2})
        process = subprocess.Popen(
            [
                binary,
                "--interval",
                "2",
                "-o",
                root,
                "tcp:127.0.0.1:PW1:good",
                "tcp:127.0.0.1:PW2:stalled",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        wait_for_reports(root, "good", [name for name in REPORTS if name != "banks"])
        if (stale / "banks.json").read_text() != "old\n":
            raise AssertionError("failed report replaced its previous file")
        time.sleep(1.2)
        stderr = stop(process)
        if "timed out" not in stderr:
            raise AssertionError(f"missing stalled-node timeout: {stderr}")

    server.join(5)
    if errors:
        raise errors[0]


def read_master(master, delimiter, timeout=5):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while not data.endswith(delimiter) and time.monotonic() < deadline:
        try:
            chunk = os.read(master, 1)
        except OSError:
            time.sleep(0.01)
            continue
        if chunk:
            data.extend(chunk)
    if not data.endswith(delimiter):
        raise RuntimeError(f"serial timeout waiting for {delimiter!r}: {data!r}")
    return bytes(data)


def serial_server(master, errors):
    try:
        if read_master(master, b"\r") != b"\r":
            raise AssertionError("missing serial prompt request")
        os.write(master, b"power4> ")
        for name in REPORTS:
            command = read_master(master, b"\r")[:-1]
            expected = f"p4exec report {name}".encode()
            if command != expected:
                raise AssertionError(f"unexpected serial command: {command!r}")
            json_text = report_json(name)
            os.write(
                master,
                command
                + b"\r\n"
                + p4j1(json_text).encode()
                + b"\r\n.\r\npower4> ",
            )
    except Exception as error:
        errors.append(error)


def run_serial_case(binary):
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.close(slave)
    errors = []
    server = threading.Thread(target=serial_server, args=(master, errors))
    server.start()
    with tempfile.TemporaryDirectory() as root:
        process = subprocess.Popen(
            [binary, "-o", root, f"serial:{path}:house"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        server.join(5)
        if server.is_alive():
            stop(process)
            raise RuntimeError("serial collection did not finish")
        if errors:
            stop(process)
            raise errors[0]
        wait_for_reports(root, "house", REPORTS)
        reopened = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        os.close(reopened)
        stop(process)
    os.close(master)


def run_busy_serial_case(binary):
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    fcntl.flock(slave, fcntl.LOCK_EX | fcntl.LOCK_NB)
    with tempfile.TemporaryDirectory() as root:
        process = subprocess.Popen(
            [binary, "--interval", "2", "-o", root, f"serial:{path}:busy"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(0.3)
        stderr = stop(process)
        if "busy" not in stderr:
            raise AssertionError(f"busy serial port was not diagnosed: {stderr}")
    os.close(slave)
    os.close(master)


def run_invalid_serial_startup_case(binary):
    path = "/definitely/not/a/power4-device"
    result = subprocess.run(
        [binary, f"serial:{path}:missing"],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    if result.returncode == 0:
        raise AssertionError("missing serial device was accepted at startup")
    if path not in result.stderr or "serial device is unavailable" not in result.stderr:
        raise AssertionError(f"missing serial diagnostic was unclear: {result.stderr}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} power4d-binary")
    binary = os.path.abspath(sys.argv[1])
    run_tcp_case(binary)
    run_oversized_line_case(binary)
    run_partial_and_concurrent_case(binary)
    run_serial_case(binary)
    run_busy_serial_case(binary)
    run_invalid_serial_startup_case(binary)
    print("multi-node TCP and serial report collection: ok")


if __name__ == "__main__":
    main()
