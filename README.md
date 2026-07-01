# power4

`power4` is firmware for a Waveshare ESP32-S3-Relay-6CH used as a configurable
power controller. The controller is intended to run unattended for years while
making relay decisions from battery state data and a site-specific policy.

The initial hardware plan is:

- Waveshare ESP32-S3-Relay-6CH as the relay controller.
- Battery management systems reporting battery state over BLE.
- A Raspberry Pi connected to the ESP32-S3 USB port for console access,
  configuration transfer, monitoring, and future maintenance tooling.
- Relay outputs assigned to equipment such as generators, DC/DC transfer
  systems, and ancillary loads.

This repository is starting from the system shape above, but the implementation
details are expected to evolve as the operational constraints become clearer.

## Goals

- Run unattended for years with predictable behavior.
- Keep the firmware simple, inspectable, and easy to reason about.
- Prefer explicit state machines, clear ownership, and small modules.
- Make configuration and operating mode changes possible through a console
  interface over USB serial.
- Support safe configuration updates from the Raspberry Pi.
- Keep the top-level developer interface behind `make`.

## Make Targets

The top-level `Makefile` is the user-facing build interface.

Common targets:

```sh
make build
make flash
make monitor
make menuconfig
make clean
```

The project target defaults to `esp32s3`. Activate ESP-IDF before running
`make`:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh"
make build
```

The Makefile can use an activated ESP-IDF environment where `idf.py` is exposed
as a shell function. If ESP-IDF is installed somewhere else, set `IDF_PY`:

```sh
make IDF_PY=/path/to/idf.py build
```

The default serial port is `/dev/tty.usbmodem1101` and can be overridden with
`PORT`:

```sh
make PORT=/dev/tty.usbmodem1101 flash monitor
```

## Configuration

Project configuration is handled through ESP-IDF Kconfig settings. Defaults live
in `sdkconfig.defaults`; the active generated configuration lives in
`sdkconfig`. Use `make menuconfig` to inspect or change settings interactively.

Relay hardware configuration is board-specific:

```text
CONFIG_POWER4_RELAY_COUNT=6
CONFIG_POWER4_RELAY_GPIO_MAP="1,2,41,42,45,46"
CONFIG_POWER4_RELAY_ACTIVE_LEVEL=1
```

`CONFIG_POWER4_RELAY_COUNT` is the number of relay outputs managed by the relay
manager.

`CONFIG_POWER4_RELAY_GPIO_MAP` is a comma-separated list of GPIO numbers in
relay-channel order. The first entry is relay 1, the second entry is relay 2,
and so on. The default map is for the Waveshare ESP32-S3-Relay-6CH:

```text
relay 1 -> GPIO 1
relay 2 -> GPIO 2
relay 3 -> GPIO 41
relay 4 -> GPIO 42
relay 5 -> GPIO 45
relay 6 -> GPIO 46
```

`CONFIG_POWER4_RELAY_ACTIVE_LEVEL` is the GPIO level that energizes a relay.
Use `1` for active-high relay drivers and `0` for active-low relay drivers.

For another board, change the relay count, GPIO map, and active level in
`sdkconfig.defaults`, then regenerate or edit `sdkconfig` and rebuild.

## Console

The initial firmware starts an ESP-IDF console REPL on the ESP32-S3 USB
Serial/JTAG console with this prompt:

```text
power4>
```

Available starter commands:

```text
help
status
system
relay
```

Relay command examples:

```text
relay list
relay query 1
relay state
relay on 1 30
relay force-on 1
relay clear-force 1
```

JSON-producing commands print a framed line with the JSON length and CRC32:

```text
P4J1 <json-length> <crc32-hex> <json>
```

## Repository Status

This is an early project skeleton. Configuration transfer, BLE battery
integration, policy execution, and safety behavior still need to be designed and
implemented.
