#!/usr/bin/env python3

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "relay-6ch": {
        "relay_kind": "gpio",
        "relay_count": "6",
        "relay_map": "1,2,41,42,45,46",
        "eth_kind": "none",
        "rtc_kind": "none",
        "input_kind": "none",
    },
    "poe-8ro": {
        "relay_kind": "tca9554",
        "relay_count": "8",
        "relay_map": "0,1,2,3,4,5,6,7",
        "tca_addr": "32",
        "i2c_sda": "42",
        "i2c_scl": "41",
        "eth_kind": "w5500",
        "eth_spi_host": "1",
        "eth_mosi": "13",
        "eth_miso": "14",
        "eth_sclk": "15",
        "eth_cs": "16",
        "eth_int": "12",
        "eth_reset": "39",
        "rtc_kind": "pcf85063a",
        "rtc_addr": "81",
        "input_kind": "gpio",
        "input_count": "8",
        "input_active": "0",
        "input_pull": "up",
        "input_map": "4,5,6,7,8,9,10,11",
    },
}


def integer(values, key):
    try:
        return int(values[key], 0)
    except (KeyError, ValueError) as error:
        raise AssertionError(f"{key} must be an integer") from error


def load_profile(name):
    path = ROOT / "board_profiles" / f"{name}.csv"
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    assert rows and rows[0]["key"] == "board" and rows[0]["type"] == "namespace"
    return {row["key"]: row["value"] for row in rows[1:]}


def validate_profile(name, expected):
    values = load_profile(name)
    assert integer(values, "schema") == 1
    assert values["profile"] == name
    assert values["model"].startswith("Waveshare ESP32-S3-")
    assert integer(values, "relay_active") in (0, 1)

    count = integer(values, "relay_count")
    channels = [int(channel) for channel in values["relay_map"].split(",")]
    assert 1 <= count <= 8
    assert len(channels) == count
    assert len(set(channels)) == count

    if values["relay_kind"] == "gpio":
        assert all(0 <= channel <= 48 for channel in channels)
    elif values["relay_kind"] == "tca9554":
        assert values["i2c_present"] == "1"
        assert all(0 <= channel < 8 for channel in channels)
        assert 0 < integer(values, "tca_addr") < 0x80
    else:
        raise AssertionError("unsupported relay backend")

    if values["eth_kind"] == "w5500":
        pins = [
            integer(values, key)
            for key in ("eth_mosi", "eth_miso", "eth_sclk", "eth_cs", "eth_int", "eth_reset")
        ]
        assert len(set(pins)) == len(pins)
        assert all(0 <= pin <= 48 for pin in pins)
        if values["i2c_present"] == "1":
            assert not set(pins) & {integer(values, "i2c_sda"), integer(values, "i2c_scl")}
        assert integer(values, "eth_spi_hz") > 0
    else:
        assert values["eth_kind"] == "none"

    if values["rtc_kind"] == "pcf85063a":
        assert values["i2c_present"] == "1"
        assert 0 < integer(values, "rtc_addr") < 0x80
        if values["relay_kind"] == "tca9554":
            assert integer(values, "rtc_addr") != integer(values, "tca_addr")
    else:
        assert values["rtc_kind"] == "none"

    if values["input_kind"] == "gpio":
        input_count = integer(values, "input_count")
        input_channels = [int(channel) for channel in values["input_map"].split(",")]
        assert 1 <= input_count <= 8
        assert len(input_channels) == input_count
        assert len(set(input_channels)) == input_count
        assert all(0 <= channel <= 48 for channel in input_channels)
        assert integer(values, "input_active") in (0, 1)
        assert values["input_pull"] in ("none", "up", "down")

        occupied = set()
        if values["relay_kind"] == "gpio":
            occupied.update(channels)
        if values["i2c_present"] == "1":
            occupied.update((integer(values, "i2c_sda"), integer(values, "i2c_scl")))
        if values["eth_kind"] == "w5500":
            occupied.update(
                integer(values, key)
                for key in ("eth_mosi", "eth_miso", "eth_sclk", "eth_cs", "eth_int", "eth_reset")
            )
        assert not set(input_channels) & occupied
    else:
        assert values["input_kind"] == "none"

    for key, expected_value in expected.items():
        assert values.get(key) == expected_value, (
            f"{name}: {key} changed from verified hardware value "
            f"{expected_value!r} to {values.get(key)!r}"
        )


def parse_size(text):
    suffixes = {"K": 1024, "M": 1024 * 1024}
    if text[-1:] in suffixes:
        return int(text[:-1], 0) * suffixes[text[-1]]
    return int(text, 0)


def validate_partition_table():
    with (ROOT / "partitions.csv").open(newline="", encoding="utf-8") as source:
        rows = {
            row[0].strip(): [field.strip() for field in row]
            for row in csv.reader(line for line in source if not line.lstrip().startswith("#"))
        }

    board = rows["board_config"]
    factory = rows["factory"]
    assert board[1:4] == ["data", "nvs", "0x1A000"]
    assert parse_size(board[4]) == 0x4000
    assert board[5] == "readonly"
    assert factory[1:3] == ["app", "factory"]
    assert factory[3] == ""
    board_end = int(board[3], 0) + parse_size(board[4])
    assert (board_end + 0xFFFF) & ~0xFFFF == 0x20000
    assert parse_size(factory[4]) == 0x100000


def validate_runtime_defaults():
    defaults = {}
    with (ROOT / "sdkconfig.defaults").open(encoding="utf-8") as source:
        for raw_line in source:
            line = raw_line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                defaults[key] = value

    event_stack = int(defaults["CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE"], 0)
    assert event_stack >= 4096, "Ethernet IP events overflow the ESP-IDF default 2304-byte stack"


def main():
    for name, expected in PROFILES.items():
        validate_profile(name, expected)
    validate_partition_table()
    validate_runtime_defaults()
    print("board profiles and partition layout: ok")


if __name__ == "__main__":
    main()
