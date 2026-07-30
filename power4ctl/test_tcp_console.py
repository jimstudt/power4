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
INPUT_JSON = (
    '{"type":"input_state","input_count":2,"inputs":['
    '{"id":1,"input_on":true},{"id":2,"input_on":false}]}'
)


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


def authenticate_client(connection):
    connection.sendall(f"authenticate {CHALLENGE}\r\n".encode())
    response = receive_until(connection, b"\n").strip().decode()
    expected = hmac.new(
        PASSWORD.encode(), CHALLENGE.encode(), hashlib.sha256
    ).hexdigest()
    if response != f"authenticate {expected}":
        raise AssertionError(f"unexpected authentication response: {response}")
    connection.sendall(b"authenticated\r\npower4> ")


def send_json_response(connection, json_text):
    digest = hashlib.sha1(json_text.encode()).hexdigest()
    connection.sendall(
        f"P4J1 {len(json_text)} {digest} {json_text}\r\n.\r\npower4> ".encode()
    )


def serve_once(errors, ready):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", PORT))
            server.listen(1)
            ready.set()
            with server.accept()[0] as connection:
                authenticate_client(connection)
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


def serve_json_once(errors, ready):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", PORT))
            server.listen(1)
            ready.set()
            with server.accept()[0] as connection:
                authenticate_client(connection)
                command = receive_until(connection, b"\r")[:-1].decode()
                if command != "p4exec report inputs":
                    raise AssertionError(f"unexpected command: {command}")
                send_json_response(connection, INPUT_JSON)
    except Exception as error:
        errors.append(error)


def run_json_case(environment):
    errors = []
    ready = threading.Event()
    server = threading.Thread(target=serve_json_once, args=(errors, ready))
    server.start()
    if not ready.wait(timeout=5):
        if errors:
            raise errors[0]
        raise RuntimeError("fake JSON server did not start")
    result = subprocess.run(
        [
            "./power4ctl",
            "-a",
            "127.0.0.1",
            "-e",
            "P4_TEST_PASSWORD",
            "json",
            "inputs",
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    server.join(timeout=5)
    if server.is_alive():
        raise RuntimeError("fake JSON server did not finish")
    if errors:
        raise errors[0]
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    if result.stdout != INPUT_JSON + "\n":
        raise AssertionError(f"unexpected JSON output: {result.stdout!r}")


def serve_daemon_cycle(errors, ready, reports):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", PORT))
            server.listen(1)
            ready.set()
            with server.accept()[0] as connection:
                authenticate_client(connection)
                for name, json_text in reports.items():
                    command = receive_until(connection, b"\r")[:-1].decode()
                    if command != f"p4exec report {name}":
                        raise AssertionError(f"unexpected daemon command: {command}")
                    send_json_response(connection, json_text)
    except Exception as error:
        errors.append(error)


def run_daemon_case(environment):
    reports = {
        "batteries": '{"type":"battery_state"}',
        "banks": '{"type":"battery_bank_state"}',
        "relays": '{"type":"relay_state"}',
        "inputs": INPUT_JSON,
        "parameters": '{"type":"policy_parameters","count":0,"parameters":[]}',
        "logs": '{"type":"logs","text":""}',
    }
    errors = []
    ready = threading.Event()
    server = threading.Thread(
        target=serve_daemon_cycle, args=(errors, ready, reports)
    )
    server.start()
    if not ready.wait(timeout=5):
        if errors:
            raise errors[0]
        raise RuntimeError("fake daemon server did not start")

    with tempfile.TemporaryDirectory() as directory:
        process = subprocess.Popen(
            [
                "./power4ctl",
                "-a",
                "127.0.0.1",
                "-e",
                "P4_TEST_PASSWORD",
                "-D",
                "-i",
                "3600",
                "-o",
                directory,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        server.join(timeout=5)
        if server.is_alive():
            process.terminate()
            process.communicate(timeout=5)
            raise RuntimeError("fake daemon server did not finish")
        if errors:
            process.terminate()
            process.communicate(timeout=5)
            raise errors[0]

        deadline = time.monotonic() + 5
        expected_paths = {
            name: pathlib.Path(directory) / f"{name}.json" for name in reports
        }
        while (
            not all(path.exists() for path in expected_paths.values())
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)

        process.terminate()
        _, stderr = process.communicate(timeout=5)
        if process.returncode != 0:
            raise AssertionError(stderr)
        for name, path in expected_paths.items():
            if not path.exists():
                raise AssertionError(f"daemon did not write {path.name}")
            if path.read_text() != reports[name] + "\n":
                raise AssertionError(f"unexpected {path.name} content")


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

    run_json_case(environment)
    run_daemon_case(environment)
    run_serial_case()
    print("dot-framed serial and authenticated TCP console clients: ok")


if __name__ == "__main__":
    main()
