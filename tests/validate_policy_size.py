#!/usr/bin/env python3

"""Keep the policy limit and its large buffers bounded and heap-backed."""

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
POLICY_HEADER = (ROOT / "main" / "policy_storage.hpp").read_text(encoding="utf-8")
POLICY_STORAGE = (ROOT / "main" / "policy_storage.cpp").read_text(encoding="utf-8")
CONSOLE = (ROOT / "main" / "console.cpp").read_text(encoding="utf-8")
POWER4CTL = (ROOT / "power4ctl" / "power4ctl.c").read_text(encoding="utf-8")

EXPECTED_BYTES = 16 * 1024


def constant_bytes(source: str, name: str) -> int:
    match = re.search(
        rf"\b{name}\b\s*(?:=|\()\s*(\d+)U?\s*\*\s*(\d+)U?",
        source,
    )
    if match is None:
        raise AssertionError(f"cannot find bounded constant {name}")
    return int(match.group(1)) * int(match.group(2))


assert constant_bytes(POLICY_HEADER, "kPolicyProgramMaxBytes") == EXPECTED_BYTES
assert constant_bytes(POWER4CTL, "POLICY_MAX_BYTES") == EXPECTED_BYTES

assert "kPolicyUploadMaxDecodedBytes = kPolicyProgramMaxBytes" in CONSOLE
assert "kConsoleRxBufferBytes = kPolicyUploadMaxEncodedBytes + 1024" in CONSOLE
assert "malloc(kPolicyUploadMaxEncodedBytes + 1)" in CONSOLE
assert "malloc(kPolicyUploadMaxDecodedBytes + 1)" in CONSOLE
assert "malloc(stored_length + 1)" in POLICY_STORAGE
assert "stored_length > kPolicyProgramMaxBytes" in POLICY_STORAGE
assert "length > kPolicyProgramMaxBytes" in POLICY_STORAGE

policy_sized_stack_array = re.search(
    r"\b(?:char|uint8_t|unsigned\s+char)\s+\w+\s*"
    r"\[\s*(?:kPolicyProgramMaxBytes|kPolicyUploadMaxDecodedBytes|"
    r"kPolicyUploadMaxEncodedBytes|POLICY_MAX_BYTES)",
    "\n".join((POLICY_STORAGE, CONSOLE, POWER4CTL)),
)
assert policy_sized_stack_array is None, "policy-sized storage must not be on a task stack"

print("policy size and heap-backed buffers: ok (16 KiB)")
