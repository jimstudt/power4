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
make package
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

To build a firmware bundle for installation from a Raspberry Pi or another
machine without ESP-IDF, run:

```sh
make package
```

This writes `dist/power4-firmware/` and `dist/power4-firmware.tar.gz`. The
bundle contains the bootloader, partition table, app binary, ESP-IDF flash
arguments, and small `flash.sh` and `monitor.sh` scripts.

The Raspberry Pi does not need a full ESP-IDF install to flash or monitor a
prebuilt bundle. A minimal setup is:

```sh
sudo apt install esptool picocom
```

After unpacking the bundle on the Pi:

```sh
PORT=/dev/ttyACM0 ./flash.sh
PORT=/dev/ttyACM0 ./monitor.sh
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
CONFIG_POWER4_MAX_BATTERIES=16
CONFIG_POWER4_MAX_BANKS=4
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

`CONFIG_POWER4_MAX_BATTERIES` is the maximum number of named batteries kept in
the in-memory observation table. If a new battery is observed when the table is
full, the least recently seen battery is evicted.

`CONFIG_POWER4_MAX_BANKS` is the maximum number of named battery banks stored in
NVS.

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
set
unset
show
bank
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

Mode/config flag examples:

```text
set generator_ok
unset generator_ok
status
```

Set names are stored as boolean flags in the `config` NVS namespace. Names are
limited to 1-15 characters: letters, digits, underscore, and hyphen.

Battery observation examples:

```text
show batteries
```

Battery observations are kept in memory by name. Each record contains voltage,
current, state of charge, and last update time. The BLE battery code will record
observations as battery integrations are added.

Battery bank examples:

```text
bank create house pack_a pack_b
bank show
bank remove house
```

Battery banks are stored persistently in the `config` NVS namespace. A bank has
a name and one or more battery names. Bank state is computed from observed
battery state: voltage is the sum of member voltages, current is the maximum
member current, and state of charge is the minimum member state of charge. If
any member battery has not been observed, the bank state is `not-ready`.

Policy execution runs from the `policy_active` NVS key. The policy task creates
a fresh Lua environment once per minute, loads the active policy, executes it,
and tears the environment down. If there is no active policy, it runs a tiny
default Lua script that prints a "no active configuration" message so the Lua
path is still exercised.

The policy Lua environment currently provides:

```lua
relay_on(1)   -- keep relay 1 on for 300 seconds
relay_off(1)  -- clear relay 1's policy timer
config_is_set("generator_ok") -- true when set from the console

ready, volts, amps, soc = battery_bank_state("house")
names = battery_bank_names()
```

Configuration command examples:

```text
config show
config show staged
config upload staged <sha1-hex>
config accept staged
```

`config upload staged` reads base64-encoded policy text from the console until a
blank line or a line containing a non-base64 character. The checksum is SHA-1 of
the decoded policy bytes, written as hexadecimal. The staged NVS key is updated
only after the decoded bytes match the requested checksum.

On a Raspberry Pi, one way to compute the checksum and prepare the upload is:

```sh
POLICY=policy.lua
SHA1=$(sha1sum "$POLICY" | awk '{print $1}')
printf 'config upload staged %s\n' "$SHA1"
base64 "$POLICY"
printf '\n'
```

Paste or send that output to the controller console. After upload:

```text
config show staged
config accept staged
```

JSON-producing commands print a framed line with the JSON length and SHA-1:

```text
P4J1 <json-length> <sha1-hex> <json>
```

BLE support is initialized with ESP-IDF NimBLE at startup. The controller
advertises as `power4` and exposes a read-only custom relay binary sensor
service. Each relay has one readable characteristic whose value is a single byte:
`0` means off and `1` means on. Timer and administrative override details remain
console-only internal state.

Relay binary sensor GATT interface:

```text
Service UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C1000

Relay 1 characteristic UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C1001
Relay 2 characteristic UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C1002
Relay N characteristic UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C1000 + N

Characteristic value: one byte, 0x00 for off or 0x01 for on.
```

Config flag GATT interface:

```text
Service UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C2000

List characteristic UUID:  79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C2001
Set characteristic UUID:   79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C2002
Unset characteristic UUID: 79C7D5F0-9A10-4A7D-8F2B-0F4A7E0C2003

List value: zero or more UTF-8 flag names separated by '\n'.
Set write value: one flag name to set.
Unset write value: one flag name to unset.
```

BLE access is currently unauthenticated. Any nearby BLE client that can connect
can read relay states, read config flags, and set or unset config flags.

## Repository Status

This is an early project skeleton. BLE battery integration, policy safety
behavior, and the actual site policy APIs still need to be designed and
implemented.

## License

This project is licensed under the MIT License. See `LICENSE`.
