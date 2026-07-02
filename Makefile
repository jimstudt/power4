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

.PHONY: all build package flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target help

all: build

build:
	$(IDF_PY) $(IDF_ARGS) build

package: build
	@mkdir -p "$(PACKAGE_DIR)/bootloader" "$(PACKAGE_DIR)/partition_table"
	@cp "$(BUILD_DIR)/bootloader/bootloader.bin" "$(PACKAGE_DIR)/bootloader/bootloader.bin"
	@cp "$(BUILD_DIR)/partition_table/partition-table.bin" "$(PACKAGE_DIR)/partition_table/partition-table.bin"
	@cp "$(BUILD_DIR)/power4.bin" "$(PACKAGE_DIR)/power4.bin"
	@sed \
		-e 's/--flash-mode/--flash_mode/g' \
		-e 's/--flash-freq/--flash_freq/g' \
		-e 's/--flash-size/--flash_size/g' \
		"$(BUILD_DIR)/flash_args" > "$(PACKAGE_DIR)/flash_args"
	@printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'PORT="$${PORT:-/dev/ttyACM0}"' \
		'BAUD="$${BAUD:-115200}"' \
		'ESPTOOL="$${ESPTOOL:-esptool}"' \
		'exec "$$ESPTOOL" --chip esp32s3 -b "$$BAUD" --before default_reset --after hard_reset --no-stub -p "$$PORT" write_flash "@flash_args"' \
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
		'  PORT=/dev/ttyACM0 ./flash.sh' \
		'' \
		'Monitor:' \
		'  PORT=/dev/ttyACM0 ./monitor.sh' \
		'' \
		'The bundle contains bootloader.bin, partition-table.bin, power4.bin, and flash_args.' \
		> "$(PACKAGE_DIR)/README.txt"
	@chmod +x "$(PACKAGE_DIR)/flash.sh" "$(PACKAGE_DIR)/monitor.sh"
	@COPYFILE_DISABLE=1 tar --format ustar -czf "$(PACKAGE_TARBALL)" -C "$$(dirname "$(PACKAGE_DIR)")" "$$(basename "$(PACKAGE_DIR)")"
	@printf 'Firmware install bundle written to %s and %s\n' "$(PACKAGE_DIR)" "$(PACKAGE_TARBALL)"

flash:
	$(IDF_PY) $(IDF_ARGS) flash

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

help:
	@printf '%s\n' \
		'power4 make targets:' \
		'  make build        Build firmware with ESP-IDF' \
		'  make package      Build and bundle binaries for Raspberry Pi flashing' \
		'  make flash        Flash firmware; set PORT=/dev/tty...' \
		'  make monitor      Open ESP-IDF serial monitor' \
		'  make menuconfig   Open ESP-IDF configuration UI' \
		'  make clean        Remove build outputs' \
		'  make fullclean    Remove all generated ESP-IDF build files' \
		'  make erase-flash  Erase target flash' \
		'  make size         Show firmware size' \
		'  make reconfigure  Regenerate build system files' \
		'  make set-target   Set ESP-IDF target, default: esp32s3' \
		'' \
		'Variables:' \
		'  IDF_PY=...        Path to idf.py, default: auto from ESP-IDF env or idf.py' \
		'  IDF_TARGET=...    ESP-IDF chip target, default: esp32s3' \
		'  PORT=...          Serial port, default: /dev/tty.usbmodem1101' \
		'  BAUD=...          Serial baud rate, default: 115200' \
		'  BUILD_DIR=...     ESP-IDF build directory, default: build' \
		'  PACKAGE_DIR=...   Firmware bundle directory, default: dist/power4-firmware'
