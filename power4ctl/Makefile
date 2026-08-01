.DEFAULT_GOAL := all

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
PROGRAM := power4ctl
LIBS    := -ledit

include ../mk/host-target.mk

ifeq ($(HOST_TARGET),native)
OUTPUT_DIR := .
else
OUTPUT_DIR := .build/$(HOST_TARGET)
endif

TARGET      := $(OUTPUT_DIR)/$(PROGRAM)
OBJS        := $(addprefix $(OUTPUT_DIR)/,power4ctl.o sha1.o sha256.o)
TEST_SHA256 := $(OUTPUT_DIR)/test_sha256
BUILD_CONFIG := Makefile ../mk/host-target.mk

BINDIR      ?= /usr/local/bin
MANDIR      ?= /usr/local/share/man/man1
SYSTEMDDIR  ?= /usr/lib/systemd/system
DEFAULTSDIR ?= /etc/default

DEB_BINDIR := /usr/bin
DEB_MANDIR := /usr/share/man/man1

.PHONY: all test install uninstall install-deb print-bin clean check-target

all: check-target $(TARGET)

check-target:
ifeq ($(HOST_TARGET),pi-trixie)
	@test -x "$(HOST_CC)" || { \
		printf 'Swift SDK clang not found: %s\n' "$(HOST_CC)" >&2; exit 1; \
	}
	@test -f "$(SWIFT_SDK_SYSROOT)/usr/include/histedit.h" || { \
		printf 'libedit headers not found in Swift SDK sysroot: %s\n' "$(SWIFT_SDK_SYSROOT)" >&2; exit 1; \
	}
	@test -f "$(SWIFT_SDK_SYSROOT)/usr/lib/aarch64-linux-gnu/libedit.so" || { \
		printf 'libedit library not found in Swift SDK sysroot: %s\n' "$(SWIFT_SDK_SYSROOT)" >&2; exit 1; \
	}
endif

$(TARGET): $(OBJS)
	$(HOST_CC) $(CFLAGS) $(HOST_LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OUTPUT_DIR)/power4ctl.o: power4ctl.c sha1.h sha256.h $(BUILD_CONFIG) | $(OUTPUT_DIR)
	$(HOST_CC) $(CFLAGS) $(HOST_CFLAGS) -c -o $@ power4ctl.c

$(OUTPUT_DIR)/sha1.o: sha1.c sha1.h $(BUILD_CONFIG) | $(OUTPUT_DIR)
	$(HOST_CC) $(CFLAGS) $(HOST_CFLAGS) -c -o $@ sha1.c

$(OUTPUT_DIR)/sha256.o: sha256.c sha256.h $(BUILD_CONFIG) | $(OUTPUT_DIR)
	$(HOST_CC) $(CFLAGS) $(HOST_CFLAGS) -c -o $@ sha256.c

$(OUTPUT_DIR):
	mkdir -p $@

$(TEST_SHA256): test_sha256.c sha256.c sha256.h $(BUILD_CONFIG) | $(OUTPUT_DIR)
	$(HOST_CC) $(CFLAGS) $(HOST_CFLAGS) $(HOST_LDFLAGS) -o $@ test_sha256.c sha256.c

test: all $(TEST_SHA256)
ifeq ($(HOST_TARGET),native)
	./$(TEST_SHA256)
	python3 test_tcp_console.py
else
	@printf 'tests cannot run locally for cross target %s\n' "$(HOST_TARGET)" >&2
	@exit 1
endif

install: all
ifeq ($(HOST_TARGET),native)
	install -m 755 $(TARGET) $(BINDIR)/$(PROGRAM)
	install -d $(MANDIR)
	gzip -c $(PROGRAM).1 > $(MANDIR)/$(PROGRAM).1.gz
else
	@printf 'install is only supported for HOST_TARGET=native\n' >&2
	@exit 1
endif

uninstall:
	rm -f $(BINDIR)/$(PROGRAM) $(MANDIR)/$(PROGRAM).1.gz

install-deb: all
	@test -n "$(DESTDIR)" || { printf 'DESTDIR is required\n' >&2; exit 1; }
	install -d "$(DESTDIR)$(DEB_BINDIR)" \
	           "$(DESTDIR)$(DEB_MANDIR)" \
	           "$(DESTDIR)$(SYSTEMDDIR)" \
	           "$(DESTDIR)$(DEFAULTSDIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(DEB_BINDIR)/$(PROGRAM)"
	gzip -c $(PROGRAM).1 > "$(DESTDIR)$(DEB_MANDIR)/$(PROGRAM).1.gz"
	install -m 644 $(PROGRAM).service "$(DESTDIR)$(SYSTEMDDIR)/$(PROGRAM).service"
	install -m 644 $(PROGRAM).default "$(DESTDIR)$(DEFAULTSDIR)/$(PROGRAM)"

print-bin:
	@printf '%s\n' "$(TARGET)"

clean:
	rm -f $(PROGRAM) power4ctl.o sha1.o sha256.o test_sha256
	rm -rf .build
