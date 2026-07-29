#!/usr/bin/env python3

import hashlib
import hmac
import os
import pathlib
import pty
import select
import socket
import subprocess
import tempfile
import threading
import time


PORT = 4244
CHALLENGE = "A" * 43
PASSWORD = "correct horse battery staple"


def receive_until(connection, delimiter):
    data = bytearray()
    while not data.endswith(delimiter):
        chunk = connection.recv(1)
        if not chunk:
            raise RuntimeError("client closed connection")
        data.extend(chunk)
    return bytes(data)


def receive_until_fd(fd, delimiter):
    data = bytearray()
    deadline = time.monotonic() + 5
    while not data.endswith(delimiter) and time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.05)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 1)
        except OSError:
            time.sleep(0.01)
            continue
        if not chunk:
            time.sleep(0.01)
            continue
        data.extend(chunk)
    if not data.endswith(delimiter):
        raise RuntimeError("timed out waiting for serial client")
    return bytes(data)


def serve_once(errors, ready):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", PORT))
            server.listen(1)
            ready.set()
            with server.accept()[0] as connection:
                connection.sendall(f"authenticate {CHALLENGE}\r\n".encode())
                response = receive_until(connection, b"\n").strip().decode()
                expected = hmac.new(
                    PASSWORD.encode(), CHALLENGE.encode(), hashlib.sha256
                ).hexdigest()
                if response != f"authenticate {expected}":
                    raise AssertionError(f"unexpected authentication response: {response}")
                connection.sendall(b"authenticated\r\npower4> ")
                command = receive_until(connection, b"\r")[:-1].decode()
                if command != "p4exec show relays":
                    raise AssertionError(f"unexpected command: {command}")
                connection.sendall(
                    b"..leading-dot\r\nrelay 1: output=off\r\n.\r\npower4> "
                )
    except Exception as error:
        errors.append(error)


def run_case(arguments, environment):
    errors = []
    ready = threading.Event()
    server = threading.Thread(target=serve_once, args=(errors, ready))
    server.start()
    if not ready.wait(timeout=5):
        if errors:
            raise errors[0]
        raise RuntimeError("fake TCP console did not start")
    result = subprocess.run(
        ["./power4ctl", "-a", "127.0.0.1", *arguments, "show", "relays"],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    server.join(timeout=5)
    if server.is_alive():
        raise RuntimeError("fake TCP console did not finish")
    if errors:
        raise errors[0]
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    if result.stdout != ".leading-dot\nrelay 1: output=off\n":
        raise AssertionError(f"unexpected output: {result.stdout!r}")


def serve_serial_once(master, errors):
    try:
        if receive_until_fd(master, b"\r") != b"\r":
            raise AssertionError("missing serial prompt request")
        os.write(master, b"power4> ")
        command = receive_until_fd(master, b"\r")[:-1]
        if command != b"p4exec show relays":
            raise AssertionError(f"unexpected serial command: {command!r}")
        os.write(
            master,
            command
            + b"\r\n..leading-dot\r\nrelay 1: output=off\r\n.\r\npower4> ",
        )
    except Exception as error:
        errors.append(error)


def run_serial_case():
    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)
    os.close(slave)
    errors = []
    server = threading.Thread(target=serve_serial_once, args=(master, errors))
    server.start()
    result = subprocess.run(
        ["./power4ctl", "-p", slave_name, "show", "relays"],
        check=False,
        capture_output=True,
        text=True,
    )
    server.join(timeout=5)
    os.close(master)
    if server.is_alive():
        raise RuntimeError("fake serial console did not finish")
    if errors:
        raise errors[0]
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    if result.stdout != ".leading-dot\nrelay 1: output=off\n":
        raise AssertionError(f"unexpected serial output: {result.stdout!r}")


def main():
    environment = os.environ.copy()
    environment["P4_TEST_PASSWORD"] = PASSWORD
    run_case(["-e", "P4_TEST_PASSWORD"], environment)

    with tempfile.TemporaryDirectory() as directory:
        password_file = pathlib.Path(directory) / "password"
        password_file.write_text(f" \t{PASSWORD}\r\n")
        run_case(["-f", str(password_file)], environment)

    run_serial_case()
    print("dot-framed serial and authenticated TCP console clients: ok")


if __name__ == "__main__":
    main()
