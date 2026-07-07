# power4

`power4` is firmware for a Waveshare ESP32-S3-Relay-6CH used as a configurable
power controller. The controller is intended to run unattended for years while
making relay decisions from battery state data and a site-specific policy.
It monitors battery state by scanning for JBD BMS advertisements over BLE and
uses that data as input to a Lua policy program that drives the relay outputs.

Also included is `power4ctl` which is a control program for a computer attached
to the USB port of the Waveshare.  It can query, control, and configure the unit.
It also can run as a daemon to keep JSON files of the current conditions.

## Make Targets

The top-level `Makefile` is the user-facing build interface.

Common targets:

```sh
make build        # build ESP32 firmware
make package      # build firmware bundle for Raspberry Pi deployment
make flash        # flash firmware over USB
make monitor      # open ESP-IDF serial monitor
make menuconfig   # open ESP-IDF configuration UI
make clean        # remove build outputs
make power4ctl    # build the host management tool
make deb          # build Debian package for power4ctl
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
CONFIG_POWER4_BATTERY_SCAN_PERIOD_SECONDS=60
CONFIG_POWER4_BATTERY_SCAN_DURATION_SECONDS=10
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

`CONFIG_POWER4_BATTERY_SCAN_PERIOD_SECONDS` and
`CONFIG_POWER4_BATTERY_SCAN_DURATION_SECONDS` control the periodic BLE battery
scanner. The scanner currently looks for JBD BMS advertisements that expose the
`0xFF00` service used with `0xFF01` and `0xFF02` characteristics.

For another board, change the relay count, GPIO map, and active level in
`sdkconfig.defaults`, then regenerate or edit `sdkconfig` and rebuild.

## Console

The firmware starts a custom line-oriented console on the ESP32-S3 USB
Serial/JTAG port. It uses ESP-IDF console command dispatch, but keeps the line
editing deliberately small so both humans and simple serial programs can drive
it reliably. The prompt is:

```text
power4>
```

Supported line editing:

```text
Backspace/Delete  erase the previous character
Ctrl-U            clear the current input line
Ctrl-R            redraw the prompt and current input line
```

ESP log output that arrives while a prompt is active is moved onto its own
line, then the prompt and any partially typed input are redrawn.

Available starter commands:

```text
help
show
set
define
remove
report
policy
reboot
```

Show command examples:

```text
show system
show relays
show ble
show batteries
show banks
show policy
show policy staged
show policy-flags
show debug
show logs
```

`show logs` prints the most recent system log text, kept in a 16 KB rolling
buffer fed by a hook on ESP logging. The buffer holds everything the firmware
logs, including policy syslog lines and policy errors, so recent history is
inspectable after the fact from the console.

Report command examples:

```text
report relays
report batteries
report logs
report banks
```

Reports are printed as a tag, byte count, SHA-1 hash, and JSON payload:

```text
P4J1 <json-bytes> <sha1-hex> <json>
```

System command examples:

```text
reboot
```

Volatile setting examples:

```text
set debug ble_scanner on
set debug ble_scanner off
set relay 1 on 30
set relay 1 force-on
set relay 1 clear-force
```

Persistent definition examples:

```text
define policy generator_ok=true
define policy gen_running=true 300s
define policy generator_ok=false
remove policy generator_ok
define bank house pack_a pack_b
show banks
remove bank house
```

Policy names are stored as boolean flags in the `config` NVS namespace. Names
are limited to 1-15 characters: letters, digits, underscore, and hyphen.
`define policy` rejects an impossible name with an explanatory error, and
`config_is_set()` in a policy program answers `false` for one (logging the
attempt) rather than aborting the policy run.

A flag may be given an optional lifetime in seconds. A lifetime flag acts as a
dead-man switch: unless it is refreshed by another `define policy` within its
lifetime, it is removed just before a policy cycle runs. Lifetimes are stored
in the `policy_ttl` NVS namespace and the countdown restarts from the full
lifetime after a reboot. `show policy-flags` reports lifetime flags as
`name(remaining/authorized)`, for example `gen_running(287/300s)`.

BLE scanner debug logging defaults to off. Turning it on prints advertisement
details, scan summaries, raw JBD basic-info packets, and decoded battery packet
details.

Battery observation examples:

```text
show batteries
report batteries
```

Battery observations are kept in memory by name. Each record contains voltage,
current, state of charge, temperature when reported, cycle count, and last
update time. The BLE battery code records observations from decoded JBD battery
packets.

Battery bank examples:

```text
define bank house pack_a pack_b
show banks
report banks
remove bank house
```

Battery banks are stored persistently in the `config` NVS namespace. A bank has
a name and one or more battery names. Bank state is computed from observed
battery state: voltage is the sum of member voltages, current is the maximum
member current, and state of charge is the minimum member state of charge. If
any member battery has not been observed, the bank state is `not-ready`.

Policy execution runs from the `policy_active` NVS key. The policy task creates
a fresh Lua environment once per minute, loads the active policy, executes it,
and tears the environment down. If there is no active policy, it runs a tiny
default Lua script that logs a "no active configuration" message so the Lua path
is still exercised.

The policy Lua environment currently provides:

```lua
relay_on(1)   -- keep relay 1 on for 300 seconds
relay_on(1, 3600) -- keep relay 1 on for an hour (1..86400 seconds)
relay_off(1)  -- clear relay 1's policy timer
on, forced, remaining = relay_state(1) -- output state, administrative force, timer seconds left
config_is_set("generator_ok") -- true when set from the console
syslog("policy reached generator_ok check") -- emit through ESP logging

ready, volts, amps, soc = battery_bank_state("house")
names = battery_bank_names()
```

Policy program command examples:

```text
show policy
show policy staged
policy upload <sha1-hex>
policy accept
```

`policy upload` reads base64-encoded policy text from the console until a
blank line or a line containing a non-base64 character. The checksum is SHA-1 of
the decoded policy bytes, written as hexadecimal. The staged NVS key is updated
only after the decoded bytes match the requested checksum.

On a Raspberry Pi, one way to compute the checksum and prepare the upload is:

```sh
POLICY=policy.lua
SHA1=$(sha1sum "$POLICY" | awk '{print $1}')
printf 'policy upload %s\n' "$SHA1"
base64 "$POLICY"
printf '\n'
```

Paste or send that output to the controller console. After upload:

```text
show policy staged
policy accept
```

`policy accept` compile-checks the staged program before activating it. A
program that does not parse is rejected with the Lua error and the current
active policy is left in place. Runtime errors can still only be discovered
live; they are reported through the policy syslog stream as
`policy error (run): ...` once per cycle.

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

## power4ctl

`power4ctl` is the host-side management tool for the controller. It lives under
`power4ctl/` and is built independently of ESP-IDF. It connects to the device
over the serial console, elicits the `power4>` prompt, issues a command, and
returns the result — all with a timeout and exclusive locking so concurrent
invocations do not collide.

### Building

From the top-level directory:

```sh
make power4ctl
```

Or directly:

```sh
make -C power4ctl
```

### Installing

```sh
make -C power4ctl install        # installs to /usr/local/bin
```

A Debian package for the current architecture can be built and installed with:

```sh
make -C power4ctl deb
sudo dpkg -i power4ctl/power4ctl_1.0.0_arm64.deb
```

### Usage

```text
power4ctl [-p port] [-b baud] [-t seconds] [-v] command [args...]
power4ctl [-p port] [-b baud] [-t seconds] [-v] -D [-i interval] [-l lock-seconds] [-o outdir]

Options:
  -p port          serial port  (default: /dev/ttyACM0)
  -b baud          baud rate    (default: 115200)
  -t seconds       timeout per operation  (default: 2)
  -v               verbose: log bytes sent/received to stderr
  -D               daemon mode: collect JSON reports on a loop
  -i seconds       daemon poll interval  (default: 60)
  -l seconds       port lock wait timeout  (default: 5)
  -o dir           daemon output directory  (default: /run/power4)
```

### Commands

**JSON reports** — connect to the device, issue the corresponding `report`
command, validate the P4J1 framed response (length + SHA-1), and print the JSON
to stdout:

```sh
power4ctl json batteries
power4ctl json banks
power4ctl json relays
```

**Policy upload** — read a policy file from disk, compute its SHA-1, send
`policy upload <sha1>` followed by the base64-encoded file and a blank line,
and report the device's confirmation:

```sh
power4ctl stage policy.lua
```

**Passthrough** — any unrecognized command is sent verbatim to the device and
all output lines are echoed to stdout until the `power4>` prompt returns. This
provides full console access without a separate terminal emulator:

```sh
power4ctl show system
power4ctl set relay 1 on 30
power4ctl policy accept
power4ctl help
```

**Daemon mode** — run indefinitely, polling the device every 60 seconds and
writing `batteries.json`, `banks.json`, and `relays.json` to `/run/power4/`.
Files are written atomically via a `.tmp.` rename so readers never see partial
content. If the port is held by another process the cycle is skipped (up to
5 s lock-wait) and the previous files are left untouched. Terminated by
`SIGTERM` or `SIGINT`:

```sh
power4ctl -D
power4ctl -D -i 30 -l 10 -o /var/lib/power4
```

### Locking

`power4ctl` uses `flock(LOCK_EX|LOCK_NB)` and `TIOCEXCL` immediately after
opening the port. In single-shot mode, if another process already holds the
port the tool exits immediately with an error. In daemon mode the lock attempt
is retried every 500 ms for up to the lock-wait timeout before the cycle is
skipped.

## Repository Status

This is an early project skeleton. BLE battery integration, policy safety
behavior, and the actual site policy APIs still need to be designed and
implemented.

## License

This project is licensed under the MIT License. See `LICENSE`.
