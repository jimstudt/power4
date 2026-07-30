# User-facing wrapper around ESP-IDF.
#
# Keep common workflows here so day-to-day development does not require
# remembering raw idf.py invocations.

ESP_ACTIVATE ?= source "$$HOME/.espressif/tools/activate_idf_v6.0.1.sh"
IDF_REQUIRED_TARGETS := all build package flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target

ifeq ($(strip $(IDF_PATH)),)
ifneq ($(filter $(IDF_REQUIRED_TARGETS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
$(error ESP-IDF environment is not active. Run: $(ESP_ACTIVATE))
endif
endif

ifneq ($(strip $(IDF_PATH)),)
ifneq ($(strip $(IDF_PYTHON_ENV_PATH)),)
IDF_PY ?= $(IDF_PYTHON_ENV_PATH)/bin/python $(IDF_PATH)/tools/idf.py
else
IDF_PY ?= $(IDF_PATH)/tools/idf.py
endif
else
IDF_PY ?= idf.py
endif
IDF_TARGET ?= esp32s3
PORT ?= /dev/tty.usbmodem1101
BAUD ?= 115200
BUILD_DIR ?= build
PACKAGE_DIR ?= dist/power4-firmware
PACKAGE_TARBALL ?= $(PACKAGE_DIR).tar.gz
SUPPORTED_BOARDS := relay-6ch poe-8ro
BOARD_CONFIG_OFFSET := 0x1a000
BOARD_CONFIG_SIZE := 0x4000
NVS_GEN_PY ?= $(if $(strip $(IDF_PYTHON_ENV_PATH)),$(IDF_PYTHON_ENV_PATH)/bin/python,python3)
NVS_GEN ?= $(IDF_PATH)/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py

export IDF_TARGET

IDF_ARGS :=
ifneq ($(strip $(PORT)),)
IDF_ARGS += -p $(PORT)
endif
ifneq ($(strip $(BAUD)),)
IDF_ARGS += -b $(BAUD)
endif
ifneq ($(strip $(BUILD_DIR)),)
IDF_ARGS += -B $(BUILD_DIR)
endif

.PHONY: all build test package flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target power4ctl power4ctl-clean deb help

all: build

build:
	$(IDF_PY) $(IDF_ARGS) -DPOWER4_BOARD= build

test:
	python3 tests/validate_board_profiles.py
	python3 tests/validate_policy_size.py
	python3 tests/validate_timezone.py
	$(MAKE) -C power4ctl test

package: build
	@mkdir -p "$(PACKAGE_DIR)/bootloader" "$(PACKAGE_DIR)/partition_table" "$(PACKAGE_DIR)/board_config"
	@cp "$(BUILD_DIR)/bootloader/bootloader.bin" "$(PACKAGE_DIR)/bootloader/bootloader.bin"
	@cp "$(BUILD_DIR)/partition_table/partition-table.bin" "$(PACKAGE_DIR)/partition_table/partition-table.bin"
	@cp "$(BUILD_DIR)/power4.bin" "$(PACKAGE_DIR)/power4.bin"
	@$(NVS_GEN_PY) "$(NVS_GEN)" generate --version 2 \
		"board_profiles/relay-6ch.csv" "$(PACKAGE_DIR)/board_config/relay-6ch.bin" "$(BOARD_CONFIG_SIZE)"
	@$(NVS_GEN_PY) "$(NVS_GEN)" generate --version 2 \
		"board_profiles/poe-8ro.csv" "$(PACKAGE_DIR)/board_config/poe-8ro.bin" "$(BOARD_CONFIG_SIZE)"
	@sed \
		-e 's/--flash-mode/--flash_mode/g' \
		-e 's/--flash-freq/--flash_freq/g' \
		-e 's/--flash-size/--flash_size/g' \
		"$(BUILD_DIR)/flash_args" > "$(PACKAGE_DIR)/flash_args"
	@printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'if [ "$$#" -ne 1 ]; then echo "usage: $$0 relay-6ch|poe-8ro" >&2; exit 2; fi' \
		'BOARD="$$1"' \
		'case "$$BOARD" in relay-6ch|poe-8ro) ;; *) echo "unknown board: $$BOARD" >&2; exit 2 ;; esac' \
		'PORT="$${PORT:-/dev/ttyACM0}"' \
		'BAUD="$${BAUD:-115200}"' \
		'ESPTOOL="$${ESPTOOL:-esptool}"' \
		'exec "$$ESPTOOL" --chip esp32s3 -b "$$BAUD" --before default_reset --after hard_reset --no-stub -p "$$PORT" write_flash "@flash_args" "$(BOARD_CONFIG_OFFSET)" "board_config/$$BOARD.bin"' \
		> "$(PACKAGE_DIR)/flash.sh"
	@printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'PORT="$${PORT:-/dev/ttyACM0}"' \
		'BAUD="$${BAUD:-115200}"' \
		'exec picocom -b "$$BAUD" "$$PORT"' \
		> "$(PACKAGE_DIR)/monitor.sh"
	@printf '%s\n' \
		'Power4 firmware install bundle' \
		'' \
		'Raspberry Pi dependencies:' \
		'  sudo apt install esptool picocom' \
		'' \
		'Flash:' \
		'  PORT=/dev/ttyACM0 ./flash.sh relay-6ch' \
		'  PORT=/dev/ttyACM0 ./flash.sh poe-8ro' \
		'' \
		'Monitor:' \
		'  PORT=/dev/ttyACM0 ./monitor.sh' \
		'' \
		'The bundle contains one shared power4.bin and one board_config image per supported board.' \
		> "$(PACKAGE_DIR)/README.txt"
	@chmod +x "$(PACKAGE_DIR)/flash.sh" "$(PACKAGE_DIR)/monitor.sh"
	@COPYFILE_DISABLE=1 tar --format ustar -czf "$(PACKAGE_TARBALL)" -C "$$(dirname "$(PACKAGE_DIR)")" "$$(basename "$(PACKAGE_DIR)")"
	@printf 'Firmware install bundle written to %s and %s\n' "$(PACKAGE_DIR)" "$(PACKAGE_TARBALL)"

flash:
	@case " $(SUPPORTED_BOARDS) " in \
		*" $(BOARD) "*) ;; \
		*) printf 'BOARD must be one of: %s\n' "$(SUPPORTED_BOARDS)" >&2; exit 2 ;; \
	esac
	$(IDF_PY) $(IDF_ARGS) -DPOWER4_BOARD=$(BOARD) flash

monitor:
	$(IDF_PY) $(IDF_ARGS) monitor

menuconfig:
	$(IDF_PY) $(IDF_ARGS) menuconfig

clean:
	$(IDF_PY) $(IDF_ARGS) clean

fullclean:
	$(IDF_PY) $(IDF_ARGS) fullclean

erase-flash:
	$(IDF_PY) $(IDF_ARGS) erase-flash

size:
	$(IDF_PY) $(IDF_ARGS) size

reconfigure:
	$(IDF_PY) $(IDF_ARGS) reconfigure

set-target:
	$(IDF_PY) $(IDF_ARGS) set-target $(IDF_TARGET)

power4ctl:
	$(MAKE) -C power4ctl

power4ctl-clean:
	$(MAKE) -C power4ctl clean

deb:
	$(MAKE) -C power4ctl deb

help:
	@printf '%s\n' \
		'power4 make targets:' \
		'  make build        Build firmware with ESP-IDF' \
		'  make test         Run board-profile and power4ctl protocol tests' \
		'  make package      Build and bundle binaries for Raspberry Pi flashing' \
		'  make flash BOARD=relay-6ch|poe-8ro' \
		'                    Flash firmware and the selected hardware profile' \
		'  make monitor      Open ESP-IDF serial monitor' \
		'  make menuconfig   Open ESP-IDF configuration UI' \
		'  make clean        Remove build outputs' \
		'  make fullclean    Remove all generated ESP-IDF build files' \
		'  make erase-flash  Erase target flash' \
		'  make size         Show firmware size' \
		'  make reconfigure  Regenerate build system files' \
		'  make set-target   Set ESP-IDF target, default: esp32s3' \
		'  make power4ctl   Build the host management tool (power4ctl/)' \
		'  make deb          Build Debian package for power4ctl' \
		'' \
		'Variables:' \
		'  IDF_PY=...        Path to idf.py, default: auto from ESP-IDF env or idf.py' \
		'  IDF_TARGET=...    ESP-IDF chip target, default: esp32s3' \
		'  PORT=...          Serial port, default: /dev/tty.usbmodem1101' \
		'  BAUD=...          Serial baud rate, default: 115200' \
		'  BUILD_DIR=...     ESP-IDF build directory, default: build' \
		'  PACKAGE_DIR=...   Firmware bundle directory, default: dist/power4-firmware' \
		'  BOARD=...         Required by make flash: relay-6ch or poe-8ro'
