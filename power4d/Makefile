.DEFAULT_GOAL := all

include ../mk/host-target.mk

PROGRAM := power4d
SWIFT_FLAGS := -c release $(SWIFT_BUILD_ARGS)
BIN_DIR = $(shell $(SWIFT) build $(SWIFT_FLAGS) --show-bin-path)
BIN = $(BIN_DIR)/$(PROGRAM)
SOURCES := Package.swift Package.resolved Makefile ../mk/host-target.mk \
	$(shell find Sources Tests -type f) test_daemon.py power4d.1 power4d.service power4d.default

.PHONY: all build test install-deb print-bin clean check-target

all: build

check-target:
ifeq ($(HOST_TARGET),pi-trixie)
	@test -d "$(SWIFT_SDK_SYSROOT)" || { \
		printf 'Swift SDK sysroot not found: %s\n' "$(SWIFT_SDK_SYSROOT)" >&2; exit 1; \
	}
	@command -v xcrun >/dev/null || { \
		printf 'xcrun is required for HOST_TARGET=pi-trixie\n' >&2; exit 1; \
	}
endif

build: check-target $(BIN)

$(BIN): $(SOURCES)
	$(SWIFT) build $(SWIFT_FLAGS)
	@touch "$(BIN)"

test: build
ifeq ($(HOST_TARGET),native)
	$(SWIFT) test
	python3 test_daemon.py "$(BIN)"
else
	@printf 'tests cannot run locally for cross target %s\n' "$(HOST_TARGET)" >&2
	@exit 1
endif

install-deb: build
	@test -n "$(DESTDIR)" || { printf 'DESTDIR is required\n' >&2; exit 1; }
	install -d "$(DESTDIR)/usr/bin" \
	           "$(DESTDIR)/usr/share/man/man1" \
	           "$(DESTDIR)/usr/lib/systemd/system" \
	           "$(DESTDIR)/etc/default"
	install -m 755 "$(BIN)" "$(DESTDIR)/usr/bin/$(PROGRAM)"
	gzip -c power4d.1 > "$(DESTDIR)/usr/share/man/man1/power4d.1.gz"
	install -m 644 power4d.service "$(DESTDIR)/usr/lib/systemd/system/power4d.service"
	install -m 600 power4d.default "$(DESTDIR)/etc/default/power4d"

print-bin:
	@printf '%s\n' "$(BIN)"

clean:
	rm -rf .build
