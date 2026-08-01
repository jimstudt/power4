# Shared target selection for the Raspberry Pi host programs.

HOST_TARGET ?= native

ifeq ($(HOST_TARGET),native)
DEB_ARCH ?= $(shell dpkg --print-architecture 2>/dev/null || \
	uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
HOST_CC ?= $(CC)
HOST_STRIP ?= strip
SWIFT ?= swift
HOST_CFLAGS :=
HOST_LDFLAGS :=
SWIFT_BUILD_ARGS :=
else ifeq ($(HOST_TARGET),pi-trixie)
DEB_ARCH := arm64
HOST_TRIPLE := aarch64-unknown-linux-gnu
SWIFT_SDK ?= swift-6.0.3-debian13-aarch64
SWIFT_SDKS_DIR ?= $(HOME)/Library/org.swift.swiftpm/swift-sdks
SWIFT_SDK_DIR ?= $(SWIFT_SDKS_DIR)/$(SWIFT_SDK).artifactbundle/$(SWIFT_SDK)/$(HOST_TRIPLE)
SWIFT_SDK_SYSROOT := $(SWIFT_SDK_DIR)/debian-trixie.sdk
SWIFT_TOOLCHAIN_BIN := $(SWIFT_SDK_DIR)/swift.xctoolchain/usr/bin
HOST_CC ?= $(SWIFT_TOOLCHAIN_BIN)/clang
HOST_STRIP ?= $(HOME)/Library/Developer/Toolchains/swift-6.3.2-RELEASE.xctoolchain/usr/bin/llvm-objcopy --strip-all
SWIFT ?= xcrun swift
HOST_CFLAGS := --target=$(HOST_TRIPLE) --sysroot=$(SWIFT_SDK_SYSROOT)
HOST_LDFLAGS := --target=$(HOST_TRIPLE) --sysroot=$(SWIFT_SDK_SYSROOT) -fuse-ld=lld
SWIFT_BUILD_ARGS := --swift-sdks-path $(SWIFT_SDKS_DIR) --swift-sdk $(SWIFT_SDK) \
	-Xlinker -rpath -Xlinker /usr/libexec/swift/lib/swift/linux
else
$(error unsupported HOST_TARGET '$(HOST_TARGET)'; supported targets: native pi-trixie)
endif
