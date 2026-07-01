# User-facing wrapper around ESP-IDF.
#
# Keep common workflows here so day-to-day development does not require
# remembering raw idf.py invocations.

ESP_ACTIVATE ?= source "$$HOME/.espressif/tools/activate_idf_v6.0.1.sh"
IDF_REQUIRED_TARGETS := all build flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target

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

.PHONY: all build flash monitor menuconfig clean fullclean erase-flash size reconfigure set-target help

all: build

build:
	$(IDF_PY) $(IDF_ARGS) build

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
		'  BUILD_DIR=...     ESP-IDF build directory, default: build'
