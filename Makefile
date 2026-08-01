# User-facing wrapper around ESP-IDF.
#
# Keep common workflows here so day-to-day development does not require
# remembering raw idf.py invocations.

.DEFAULT_GOAL := all

include mk/host-target.mk

ESP_ACTIVATE ?= source "$$HOME/.espressif/tools/activate_idf_v6.0.1.sh"
IDF_REQUIRED_TARGETS := all build package firmware-images deb flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target

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
VERSION := $(strip $(shell cat version.txt))
ifeq ($(VERSION),)
$(error cannot read version from version.txt)
endif
DEB_STAGE := dist/deb/power4_$(VERSION)_$(DEB_ARCH)
DEB_FILE := dist/power4_$(VERSION)_$(DEB_ARCH).deb
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

.PHONY: all build test package firmware-images flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target \
	power4ctl power4d host power4ctl-clean power4d-clean host-clean deb check-host-target check-deb-target help

all: build

build:
	$(IDF_PY) $(IDF_ARGS) -DPOWER4_BOARD= build

test:
	python3 tests/validate_board_profiles.py
	python3 tests/validate_policy_size.py
	python3 tests/validate_timezone.py
	$(MAKE) -C power4ctl test
	$(MAKE) -C power4d test

firmware-images: build
	@mkdir -p "$(PACKAGE_DIR)/bootloader" "$(PACKAGE_DIR)/partition_table" "$(PACKAGE_DIR)/board_config"
	@cp "$(BUILD_DIR)/bootloader/bootloader.bin" "$(PACKAGE_DIR)/bootloader/bootloader.bin"
	@cp "$(BUILD_DIR)/partition_table/partition-table.bin" "$(PACKAGE_DIR)/partition_table/partition-table.bin"
	@cp "$(BUILD_DIR)/power4.bin" "$(PACKAGE_DIR)/power4.bin"
	@$(NVS_GEN_PY) "$(NVS_GEN)" generate --version 2 \
		"board_profiles/relay-6ch.csv" "$(PACKAGE_DIR)/board_config/relay-6ch.bin" "$(BOARD_CONFIG_SIZE)"
	@$(NVS_GEN_PY) "$(NVS_GEN)" generate --version 2 \
		"board_profiles/poe-8ro.csv" "$(PACKAGE_DIR)/board_config/poe-8ro.bin" "$(BOARD_CONFIG_SIZE)"
	@$(NVS_GEN_PY) -m esptool --chip esp32s3 merge-bin \
		--flash-mode dio --flash-freq 80m --flash-size 2MB \
		-o "$(PACKAGE_DIR)/relay-6ch.bin" \
		0x0 "$(BUILD_DIR)/bootloader/bootloader.bin" \
		0x8000 "$(BUILD_DIR)/partition_table/partition-table.bin" \
		$(BOARD_CONFIG_OFFSET) "$(PACKAGE_DIR)/board_config/relay-6ch.bin" \
		0x20000 "$(BUILD_DIR)/power4.bin"
	@$(NVS_GEN_PY) -m esptool --chip esp32s3 merge-bin \
		--flash-mode dio --flash-freq 80m --flash-size 2MB \
		-o "$(PACKAGE_DIR)/poe-8ro.bin" \
		0x0 "$(BUILD_DIR)/bootloader/bootloader.bin" \
		0x8000 "$(BUILD_DIR)/partition_table/partition-table.bin" \
		$(BOARD_CONFIG_OFFSET) "$(PACKAGE_DIR)/board_config/poe-8ro.bin" \
		0x20000 "$(BUILD_DIR)/power4.bin"
	@printf 'Board firmware images written to %s/relay-6ch.bin and %s/poe-8ro.bin\n' \
		"$(PACKAGE_DIR)" "$(PACKAGE_DIR)"

package: firmware-images
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
		'Complete image:' \
		'  esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 relay-6ch.bin' \
		'  esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 poe-8ro.bin' \
		'' \
		'Monitor:' \
		'  PORT=/dev/ttyACM0 ./monitor.sh' \
		'' \
		'The bundle contains one shared power4.bin and one board_config image per supported board.' \
		'The top-level relay-6ch.bin and poe-8ro.bin files are complete images flashed at offset 0.' \
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
	$(MAKE) -C power4ctl HOST_TARGET="$(HOST_TARGET)"

power4d:
	$(MAKE) -C power4d HOST_TARGET="$(HOST_TARGET)"

host: check-host-target power4ctl power4d

power4ctl-clean:
	$(MAKE) -C power4ctl clean

power4d-clean:
	$(MAKE) -C power4d clean

host-clean: power4ctl-clean power4d-clean

check-host-target:
ifeq ($(HOST_TARGET),pi-trixie)
	@test -d "$(SWIFT_SDK_SYSROOT)" || { \
		printf 'Swift SDK sysroot not found: %s\n' "$(SWIFT_SDK_SYSROOT)" >&2; exit 1; \
	}
	@test -x "$(HOST_CC)" || { \
		printf 'Swift SDK clang not found: %s\n' "$(HOST_CC)" >&2; exit 1; \
	}
	@test -x "$(word 1,$(HOST_STRIP))" || { \
		printf 'ELF strip tool not found: %s\n' "$(word 1,$(HOST_STRIP))" >&2; exit 1; \
	}
endif

check-deb-target: check-host-target
ifeq ($(HOST_TARGET),native)
	@case "$(DEB_ARCH)" in \
		arm64|amd64) ;; \
		*) printf 'native Debian packaging requires Linux; detected architecture: %s\n' "$(DEB_ARCH)" >&2; exit 1 ;; \
	esac
endif

deb: firmware-images check-deb-target host
	rm -rf "$(DEB_STAGE)"
	install -d "$(DEB_STAGE)/DEBIAN"
	$(MAKE) -C power4ctl HOST_TARGET="$(HOST_TARGET)" DESTDIR="$(abspath $(DEB_STAGE))" install-deb
	$(MAKE) -C power4d HOST_TARGET="$(HOST_TARGET)" DESTDIR="$(abspath $(DEB_STAGE))" install-deb
	install -d "$(DEB_STAGE)/usr/share/power4/firmware"
	install -m 644 "$(PACKAGE_DIR)/relay-6ch.bin" \
		"$(DEB_STAGE)/usr/share/power4/firmware/relay-6ch.bin"
	install -m 644 "$(PACKAGE_DIR)/poe-8ro.bin" \
		"$(DEB_STAGE)/usr/share/power4/firmware/poe-8ro.bin"
	$(HOST_STRIP) "$(DEB_STAGE)/usr/bin/power4ctl"
	$(HOST_STRIP) "$(DEB_STAGE)/usr/bin/power4d"
	sed -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@ARCH@/$(DEB_ARCH)/g' \
	    debian/control.in > "$(DEB_STAGE)/DEBIAN/control"
	install -m 755 debian/postinst "$(DEB_STAGE)/DEBIAN/postinst"
	install -m 755 debian/prerm "$(DEB_STAGE)/DEBIAN/prerm"
	cp debian/conffiles "$(DEB_STAGE)/DEBIAN/conffiles"
	dpkg-deb --build --root-owner-group "$(DEB_STAGE)" "$(DEB_FILE)"
	rm -rf "$(DEB_STAGE)"
	@printf 'Debian package written to %s\n' "$(DEB_FILE)"

help:
	@printf '%s\n' \
		'power4 make targets:' \
		'  make build        Build firmware with ESP-IDF' \
		'  make test         Run firmware logic and host-program tests' \
		'  make package      Build and bundle binaries for Raspberry Pi flashing' \
		'  make firmware-images' \
		'                    Build complete relay-6ch and poe-8ro flash images' \
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
		'  make power4ctl    Build the host management tool (power4ctl/)' \
		'  make power4d      Build the Swift multi-node collector (power4d/)' \
		'  make host         Build both Raspberry Pi host programs' \
		'  make host-clean   Remove both host programs build outputs' \
		'  make deb          Build firmware and one power4 package with both programs' \
		'' \
		'Variables:' \
		'  IDF_PY=...        Path to idf.py, default: auto from ESP-IDF env or idf.py' \
		'  IDF_TARGET=...    ESP-IDF chip target, default: esp32s3' \
		'  PORT=...          Serial port, default: /dev/tty.usbmodem1101' \
		'  BAUD=...          Serial baud rate, default: 115200' \
		'  BUILD_DIR=...     ESP-IDF build directory, default: build' \
		'  PACKAGE_DIR=...   Firmware bundle directory, default: dist/power4-firmware' \
		'  BOARD=...         Required by make flash: relay-6ch or poe-8ro' \
		'  HOST_TARGET=...   Host target: native (default) or pi-trixie' \
		'  SWIFT_SDKS_DIR=... Override the directory containing Swift SDK bundles' \
		'  SWIFT_SDK_DIR=... Override the installed Swift SDK variant directory'
