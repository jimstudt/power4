# power4

`power4` is firmware for Waveshare ESP32-S3 relay controllers used as
configurable power controllers. One firmware image supports:

- `relay-6ch`: Waveshare ESP32-S3-Relay-6CH
- `poe-8ro`: Waveshare ESP32-S3-POE-ETH-8DI-8RO

The controller is intended to run unattended for years while making relay
decisions from battery state data and a site-specific policy.
It monitors battery state by scanning for JBD BMS advertisements over BLE and
uses that data as input to a Lua policy program that drives the relay outputs.

Also included is `power4ctl` which is a control program for a computer attached
to the USB port or authenticated TCP console of the controller. It can query,
control, and configure one unit. `power4d` is a Swift daemon that concurrently
collects JSON reports from multiple serial or TCP controllers.

## Make Targets

The top-level `Makefile` is the user-facing build interface.

Common targets:

```sh
make build        # build ESP32 firmware
make package      # build firmware bundle for Raspberry Pi deployment
make flash BOARD=relay-6ch  # flash firmware and the selected board profile
make monitor      # open ESP-IDF serial monitor
make menuconfig   # open ESP-IDF configuration UI
make clean        # remove build outputs
make power4ctl    # build the host management tool
make power4d      # build the Swift host program
make host         # build both host programs
make deb          # build one Debian package containing both host programs
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
make PORT=/dev/tty.usbmodem1101 BOARD=relay-6ch flash monitor
```

Flashing requires a board name so the correct hardware profile is written:

```sh
make PORT=/dev/tty.usbmodem1101 flash BOARD=relay-6ch
make PORT=/dev/tty.usbmodem1101 flash BOARD=poe-8ro
```

To build a firmware bundle for installation from a Raspberry Pi or another
machine without ESP-IDF, run:

```sh
make package
```

This writes `dist/power4-firmware/` and `dist/power4-firmware.tar.gz`. The
bundle contains the bootloader, partition table, app binary, ESP-IDF flash
arguments, complete `relay-6ch.bin` and `poe-8ro.bin` images, and small
`flash.sh` and `monitor.sh` scripts. A complete board image is flashed at
offset zero.

The factory application partition is 1 MiB. Installations running the former
1792 KiB layout should flash the supplied partition table along with the
application; the normal `make flash` and packaged `flash.sh` commands already
do this.

The Raspberry Pi does not need a full ESP-IDF install to flash or monitor a
prebuilt bundle. A minimal setup is:

```sh
sudo apt install esptool picocom
```

After unpacking the bundle on the Pi:

```sh
PORT=/dev/ttyACM0 ./flash.sh relay-6ch
PORT=/dev/ttyACM0 ./flash.sh poe-8ro
PORT=/dev/ttyACM0 ./monitor.sh
```

## Configuration

Project configuration is handled through ESP-IDF Kconfig settings. Defaults live
in `sdkconfig.defaults`; the active generated configuration lives in
`sdkconfig`. Use `make menuconfig` to inspect or change settings interactively.

Board hardware configuration is separate from normal settings. It lives in the
read-only `board_config` NVS partition at flash offset `0x1a000`; the profile
CSV sources are in `board_profiles/`. The selected profile describes relay
count and backend, digital inputs, I2C, Ethernet, and RTC hardware. The
application image is identical for both boards.

The shipped profiles are:

| Profile | Relays | Digital inputs | Ethernet | RTC |
| --- | --- | --- | --- | --- |
| `relay-6ch` | Six active-high GPIO outputs: 1, 2, 41, 42, 45, 46 | None | None | None |
| `poe-8ro` | Eight active-high TCA9554 outputs, bits 0–7 at I2C address `0x20` | Eight active-low GPIO inputs: 4–11, with pull-ups | W5500 over SPI | PCF85063A at I2C address `0x51` |

If the profile partition is missing, unreadable, or invalid, firmware starts
the USB console but does not start relay control, BLE scanning, or policy
execution. This fail-closed behavior prevents an unknown board layout from
energizing an output.

Normal capacity and scanner settings remain in Kconfig:

```text
CONFIG_POWER4_MAX_RELAYS=8
CONFIG_POWER4_MAX_BATTERIES=16
CONFIG_POWER4_MAX_BANKS=4
CONFIG_POWER4_BATTERY_SCAN_PERIOD_SECONDS=60
CONFIG_POWER4_BATTERY_SCAN_DURATION_SECONDS=10
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

`CONFIG_POWER4_MAX_RELAYS` sizes bounded firmware storage. It must be at least
as large as the relay count in every supported board profile.

`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` is raised above the ESP-IDF default
because Ethernet DHCP/IP event processing and firmware log capture share the
`sys_evt` task stack.

`CONFIG_POWER4_MAX_BATTERIES` is the maximum number of named batteries kept in
the in-memory observation table. If a new battery is observed when the table is
full, the least recently seen battery is evicted.

`CONFIG_POWER4_MAX_BANKS` is the maximum number of named battery banks stored in
NVS.

`CONFIG_POWER4_BATTERY_SCAN_PERIOD_SECONDS` and
`CONFIG_POWER4_BATTERY_SCAN_DURATION_SECONDS` control the periodic BLE battery
scanner. The scanner currently looks for JBD BMS advertisements that expose the
`0xFF00` service used with `0xFF01` and `0xFF02` characteristics.

To add another board, add a versioned profile CSV, extend the supported-board
list in the Makefile/CMake validation, and implement any new bounded hardware
backend required by that profile.

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
show board
show ethernet
show inputs
show password
show policy
show policy staged
show policy parameters
show debug
show logs
show time
show timezone
show timezones
```

`show logs` prints the most recent system log text, kept in a 16 KB rolling
buffer fed by a hook on ESP logging. The buffer holds everything the firmware
logs, including policy syslog lines and policy errors, so recent history is
inspectable after the fact from the console.

Report command examples:

```text
report relays
report inputs
report parameters
report batteries
report logs
report banks
```

Reports are printed as a tag, byte count, SHA-1 hash, and JSON payload:

```text
P4J1 <json-bytes> <sha1-hex> <json>
```

`report parameters` returns the currently defined policy parameters in
alphabetical order. Each entry contains `name`, `value_type`, typed `value`,
the original `value_text`, `lifetime_s`, and `remaining_s`. The report also
includes the bounded snapshot capacity and a `truncated` flag.

System command examples:

```text
reboot
```

Setting examples:

```text
set debug ble_scanner on
set debug ble_scanner off
set ethernet dhcp
set ethernet static 192.168.10.20 255.255.255.0 192.168.10.1
set ethernet static 192.168.10.20 255.255.255.0 192.168.10.1 1.1.1.1 8.8.8.8
set ethernet phy auto
set ethernet phy 10-full
set password
set password "a manually chosen password"
set relay 1 on 30
set relay 1 force-on
set relay 1 force-off
set relay 1 clear-force
set timezone US/Central
set time 2026-07-29 19:30:00
```

Ethernet address and PHY settings are stored in the normal writable NVS
partition and applied immediately. Addressing may be DHCP or static IPv4. PHY
modes are `auto`, `10-half`, `10-full`, `100-half`, and `100-full`. Ethernet
commands report `not present` on the 6-channel board.

`set password` with no argument generates a 43-character base64url password
from 32 random bytes, saves it in NVS, and prints it. Supplying an argument
sets that 16–128 byte printable password instead. `show password` reveals the
stored value. Both password commands and all `set ethernet` commands are
restricted to the physical serial console.

On Ethernet boards, an authenticated TCP console listens on IPv4 port 4244.
The server sends a random `authenticate` challenge, and the client proves it
knows the password with HMAC-SHA256. After authentication, the session is
clear text and also accepts ordinary human console commands. Commands issued
by `power4ctl` use the tool framing described below. A connection has a
five-second authentication/input timeout, a two-second send timeout, a
15-second total command-I/O deadline, and a 60-second idle timeout so a failed
network client cannot hold the command processor indefinitely.

`show inputs` reports the raw GPIO level and logical on/off state for every
configured digital input. Boards without digital inputs report `not present`.
Inputs are sampled when requested; firmware does not currently debounce them
or attach actions directly to input edges.

The RTC stores UTC. At boot, a valid RTC seeds the POSIX system clock. On an
Ethernet board, the firmware then starts an SNTP client using Cloudflare's
anycast NTP server at `162.159.200.123`; each successful synchronization
updates the system clock and writes UTC back to the RTC. If the RTC is unset,
the system clock remains invalid until SNTP succeeds.

`set time` accepts a UTC timestamp and updates both the RTC and the system
clock. `show time` prints UTC and local civil time plus the clock source.
`set timezone US/Central` persists the timezone in NVS and applies the POSIX
rule `CST6CDT,M3.2.0/2,M11.1.0/2`, including automatic modern US daylight
saving transitions. `UTC` is the default. `show timezones` lists the 30
supported single-token human names with their three- or four-letter standard
abbreviation and POSIX rule; use one of those human names with `set timezone`.
`show timezone` displays the selected name, POSIX rule, current abbreviation,
and SNTP state. After installing firmware with the UTC convention on a unit
whose RTC was previously set to local wall time, set the clock to UTC once.

A forced relay ignores its policy timer: `force-off` keeps the relay open no
matter what policy does (for example, while working on wiring) and `force-on`
holds it closed. A relay holds one force at a time; setting a force replaces
the previous one and `clear-force` returns the relay to timer control.

Persistent definition examples:

```text
define policy generator_ok=true
define policy gen_running=true 300s
define policy generator_ok=false
define policy b24_low_limit=40
define policy soc_target=87.5
remove policy generator_ok
define bank house pack_a pack_b
show banks
remove bank house
```

Policy parameters are stored in the `config` NVS namespace. Names are
limited to 1-15 characters: letters, digits, underscore, and hyphen.
`define policy` rejects an impossible name with an explanatory error, and
the config readers in a policy program answer their default for one
(logging the attempt) rather than aborting the policy run.

A policy parameter is either a boolean or a number. Numbers may be integers
or floats (`40`, `-3`, `87.5`, `1e3`). Every parameter is stored as an NVS
string of its text form — `true`, `false`, or the numeric text — so values
are inspectable, `show policy parameters` reports them as `name=value`, and
a policy program reads numbers back with the integer/float distinction
intact. A name holds one value: defining a number replaces a boolean of the
same name and vice versa. Spaces around the `=` are accepted.

`define policy <name>=false` stores an explicit false — it does not remove
the parameter — so a policy program can distinguish "set false" from "never
defined" and apply a default (see `config_bool()` below). `remove policy
<name>` returns a name to undefined.

A parameter may be given an optional lifetime in seconds. A lifetime
value acts as a dead-man switch: unless it is refreshed by another
`define policy` within its lifetime, it is removed just before a policy
cycle runs. Lifetimes are stored
in the `policy_ttl` NVS namespace and the countdown restarts from the full
lifetime after a reboot. `show policy parameters` lists parameters one per
line in alphabetical order and reports lifetime values as
`name=value (remaining/authorized)`, for example `gen_running=true (287/300s)`.

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
on, force, remaining = relay_state(1) -- output state, force ("on"/"off"/nil), timer seconds left
input_on(1) -- true when configured digital input 1 is asserted
now = rtc_time() -- UTC table: year, month, day, weekday (0=Sunday), hour, minute, second, valid, oscillator_stopped, utc
local_now = local_time() -- timezone-adjusted system time with valid, utc_offset_minutes, daylight_saving, zone, and timezone
config_is_set("generator_ok") -- true only when defined true
config_bool("allow-generator", true) -- boolean parameter, or the default (nil if omitted) when unset
config_number("b24_low_limit", 40) -- numeric parameter, or the default (nil if omitted) when unset
syslog("policy reached generator_ok check") -- emit through ESP logging

ready, volts, amps, soc = battery_bank_state("house")
names = battery_bank_names()
```

`input_on()` raises a policy error when the requested input is not configured.
`rtc_time()` raises a policy error when the RTC is absent or cannot be read.
When the RTC can be read but has reported an oscillator stop, the returned
table has `valid=false` and `oscillator_stopped=true`.
`local_time()` uses the configured POSIX timezone and returns `valid=false`
until the system clock has been seeded from the RTC, set manually, or
synchronized through SNTP.

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
only after the decoded bytes match the requested checksum. Policy source is
limited to 16 KiB. The full-size upload, stored-policy read, and active-policy
source buffers are heap allocated; policy-sized arrays are not placed on a
FreeRTOS task stack.

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

## Host tools

`power4ctl` is the host-side management tool for the controller. It lives under
`power4ctl/` and is built independently of ESP-IDF. It connects over USB serial
or the authenticated TCP console, elicits the `power4>` prompt, issues a
tool-framed command, and returns the result.

For both transports, the tool sends `p4exec <command>`. Firmware streams the
command output without accumulating a complete response, doubles a leading
dot on any output line, and terminates the response with `.` on a line by
itself. `power4ctl` removes the dot stuffing and does not use the interactive
prompt as an end-of-response marker. Direct human console commands remain
unchanged.

`power4d` lives under `power4d/` and is built with SwiftPM. It is the
systemd-managed report collector. Each configured node has its own connection
method and output directory, and nodes are polled concurrently.

### Building

From the top-level directory:

```sh
make host
```

Each program can also be built separately:

```sh
make -C power4ctl
make -C power4d
```

To cross-compile both programs for 64-bit Raspberry Pi OS Trixie using the
installed Swift SDK:

```sh
make host HOST_TARGET=pi-trixie
```

The SDK defaults to `swift-6.0.3-debian13-aarch64`. Set `SWIFT_SDKS_DIR` if the
bundle is installed somewhere other than SwiftPM's standard
`~/Library/org.swift.swiftpm/swift-sdks` location. `SWIFT_SDK_DIR` can override
the derived target-variant directory used by the C cross compiler.

### Installing

```sh
make -C power4ctl install        # installs to /usr/local/bin
```

A single Debian package containing both host programs can be built natively on
Debian or cross-built on macOS:

```sh
make deb                         # native Debian build
make deb HOST_TARGET=pi-trixie  # ARM64 Trixie cross build
sudo apt install "./dist/power4_$(cat version.txt)_arm64.deb"
```

Both package commands also rebuild the firmware and merged board images, so an
ESP-IDF environment must be active even when the host programs are being
cross-compiled.

The package depends on Debian Trixie's Swift 6.0.3 runtime package,
`libswiftlang`, which installs the required libraries under
`/usr/libexec/swift/lib/swift/linux`. Installing `power4` replaces the former
`power4ctl` Debian package while retaining the `power4ctl` command. Report
collection is owned by `power4d.service`; the former `power4ctl.service` and
`/etc/default/power4ctl` are retired without translating their single-node
settings.
It also installs complete board-specific firmware images under
`/usr/share/power4/firmware`. For example:

```sh
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 \
  /usr/share/power4/firmware/poe-8ro.bin
```

Use `relay-6ch.bin` instead for the six-relay board. `esptool` is an optional
deployment tool and is not a dependency of the Debian package.

### `power4ctl` usage

```text
power4ctl [-p port | -a address (-e name | -f file)] [-b baud] [-t seconds] [-v] command [args...]
power4ctl [-p port | -a address (-e name | -f file)] [-b baud] [-t seconds] [-v]

Options:
  -p port          serial port  (default: /dev/ttyACM0)
  -a address       use authenticated TCP console at IPv4 address
  -e name          read TCP password from environment variable name
  -f file          read TCP password from file, trimming whitespace
  -b baud          baud rate    (default: 115200)
  -t seconds       timeout per operation  (default: 2)
  -v               verbose: log bytes sent/received to stderr
```

### Commands

**JSON reports** — connect to the device, issue the corresponding `report`
command, validate the P4J1 framed response (length + SHA-1), and print the JSON
to stdout:

```sh
power4ctl json batteries
power4ctl json banks
power4ctl json inputs
power4ctl json logs
power4ctl json parameters
power4ctl json relays
```

**Policy upload** — read a policy file from disk, compute its SHA-1, send
`policy upload <sha1>` followed by the base64-encoded file and a blank line,
and report the device's confirmation:

```sh
power4ctl stage policy.lua
```

**Passthrough** — any unrecognized command is wrapped as a tool command and
its dot-framed output is echoed to stdout. This provides full console access
without a separate terminal emulator:

```sh
power4ctl show system
power4ctl set relay 1 on 30
power4ctl policy accept
power4ctl help
```

TCP examples:

```sh
POWER4_PASSWORD='the password shown by set password'
export POWER4_PASSWORD
power4ctl -a 10.10.10.163 -e POWER4_PASSWORD show relays

power4ctl -a 10.10.10.163 -f /etc/power4/password json batteries
```

Exactly one password source is required with `-a`. Environment values are
used exactly; password files have leading and trailing whitespace removed.
Authentication protects access to the session, but commands and responses
after login are clear text on the LAN.

**Interactive REPL** — invoked with no command, enters a line-editing shell
(powered by libedit) with Emacs key bindings and persistent command history
in `~/.power4ctl_history`. The serial port is opened for each command and
released before the next prompt, so other tools can interleave. Type `exit`,
`quit`, or press Ctrl-D to leave:

```sh
power4ctl
power4ctl -p /dev/ttyACM1
power4ctl -a 10.10.10.163 -f ~/.config/power4/password
```

### `power4d` report collection

`power4d` requires at least one positional node specification. Serial device
paths use their final colon as the separator, so paths containing colons are
accepted:

```sh
power4d [--interval seconds] [--baud rate] \
  [--output-directory path] [--verbose] node [node...]

PW1='controller password' power4d \
  tcp:10.10.10.3:PW1:shed \
  serial:/dev/serial/by-id/example:house
```

TCP nodes use authenticated console port 4244. Their password comes from the
named environment variable, so the password value is not exposed in process
arguments. Hostnames resolve to cached IPv4 addresses at startup. Each TCP
node's full six-report cycle has a deadline of half the polling interval.

The six files are `batteries.json`, `banks.json`, `relays.json`, `inputs.json`,
`parameters.json`, and `logs.json`. They are atomically replaced beneath
`<output-directory>/<node>/`; a failed or uncollected report leaves its prior
file intact. Nodes start immediately and run concurrently without overlapping
their own previous cycle.

At startup, every configured serial path must exist, resolve to a character
device, and be readable and writable. A configuration error terminates the
daemon with the failing path in the diagnostic. Ports that become unavailable
or busy after startup are still skipped and retried normally.

The Debian service reads `/etc/default/power4d`, installed mode `0600`:

```sh
INTERVAL=60
BAUD=115200
OUTDIR=/run/power4
POWER4D_OPTIONS=
NODES="tcp:10.10.10.3:PW1:shed serial:/dev/serial/by-id/example:house"
PW1="controller password"
```

The package enables the service but does not start a fresh installation until
nodes are configured. After editing the defaults file, start it with:

```sh
sudo systemctl start power4d.service
```

### Locking

For serial connections, `power4ctl` uses `flock(LOCK_EX|LOCK_NB)` and
`TIOCEXCL` immediately after opening the port. If another process already holds
the port the tool exits immediately with an error. `power4d` uses the same
exclusion mechanisms, skips a busy serial node immediately, and holds the port
only across that node's six reports. TCP sessions rely on the firmware's single
authenticated session and command serialization.

On macOS, `power4ctl` transparently maps a `/dev/tty.*` serial path to its
matching `/dev/cu.*` callout device. This avoids the dial-in device's blocking
carrier wait while preserving existing commands and configuration.

## Example Policies

`examples/house.lua` is the house load-shedding policy. It coordinates the
Ethernet, admin-computer, internet, powered-Ethernet, and porch-camera relays
from externally maintained power, force, and deep-sleep flags plus two
physical mode inputs. It uses `local_time()` and the configured system
timezone to compute daylight, sunrise, noon, and sunset windows for latitude
45.127778, longitude -87.246944. No seasonal policy flag is needed. The
sunrise, local-noon, and sunset windows span five minutes before through five
minutes after each event. Its precedence and overlap behavior are covered by
`examples/house_test.lua`. DI1 is the occupied switch and powers the five named
loads; DI2 requests all eight relay channels:

```sh
lua examples/house_test.lua examples/house.lua
```

`examples/shed.lua` is a complete site policy managing a 48v bank charged by
a generator and a pair of paralleled 24v banks fed from the 48v bank through
a DC/DC converter, with hysteresis, deadman holds, manual override flags,
and tunable thresholds read from policy parameters. Relay 4 powers the camera
PoE switch when the `enableCameras` policy boolean is true.
`examples/shed_test.lua` runs it against scripted scenarios on a host with a
stock Lua 5.4 interpreter:

```sh
lua5.4 examples/shed_test.lua examples/shed.lua
```

## Repository Status

The firmware and `power4ctl` are in service running a real installation:
BLE battery monitoring, battery banks, the Lua policy engine, policy
parameters with lifetimes, relay deadman holds, log capture, and JSON
reporting are all
functional. BLE access remains unauthenticated, and policy staging is
console-only by design.

## License

This project is licensed under the MIT License. See `LICENSE`.
