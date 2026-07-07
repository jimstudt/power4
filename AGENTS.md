# AGENTS.md

Guidance for agents and contributors working in this repository.

## Project Intent

This firmware controls real power equipment through relays. Treat every change
as if it may eventually run unattended in a physical installation.

Optimize for:

- correctness that is easy to inspect,
- conservative failure behavior,
- simple control flow,
- small modules with explicit responsibilities,
- field maintainability.

Avoid:

- clever abstractions in safety-relevant paths,
- hidden allocation or unbounded work in periodic control code,
- large framework-style rewrites,
- broad dependencies without a clear operational payoff.

## Build Interface

Use the top-level `Makefile` for normal interactions. Do not require users to
remember raw ESP-IDF commands for common work.

Expected entry points:

```sh
make build
make flash
make monitor
make menuconfig
make clean
```

Keep new developer workflows behind Make targets when they become routine.

## ESP-IDF / FreeRTOS Conventions

- Prefer ESP-IDF APIs and idioms over custom platform wrappers unless a wrapper
  makes hardware or policy boundaries clearer.
- Keep FreeRTOS task ownership explicit. Name tasks by responsibility.
- Avoid sharing mutable state between tasks without a clear synchronization
  strategy.
- Prefer queues, event groups, and small immutable snapshots over scattered
  globals.
- Keep ISR work minimal.
- Make watchdog behavior intentional and documented near the task setup.

## C and C++ Style

C is fine. C++ is fine when it makes code clearer.

Use C++ as "nicer C with objects":

- explicit constructors or init functions,
- simple value types,
- narrow interfaces,
- no inheritance unless it is plainly useful,
- no template-heavy designs,
- no Java-style service hierarchies,
- no broad STL use in firmware control paths.

Prefer fixed-size storage and bounded operations. If dynamic allocation is used,
make the lifetime and failure behavior obvious.

## Control Logic

The planned policy loop runs once per minute and decides relay intent from
system state and configuration. Keep this boundary crisp:

- inputs are explicit snapshots,
- outputs are desired relay states and logged reasons,
- hardware application is separate from policy evaluation.

Policy failures must produce conservative, inspectable behavior.

## Console and Configuration

The Raspberry Pi is expected to connect over USB serial. Console commands should
be stable, scriptable, and human-readable.

When implementing configuration transfer:

- validate before activating,
- preserve the last known good configuration when possible,
- make activation explicit,
- report parse, validation, and runtime errors clearly,
- avoid bricking the controller with a bad upload.

## power4ctl Notes

`LINEBUF` in `power4ctl/power4ctl.c` is deliberately huge (128 KB). A P4J1
frame is a single line, and `report logs` carries the device's 16 KB log
buffer as one JSON string that can grow to roughly 6x under `\uXXXX`
escaping, so the line buffer must hold the worst case. The buffers are
static and power4ctl runs on a Raspberry Pi, so this is currently free. If
RAM ever gets tight there, address it here: chunk the logs report, stream
the frame parse, or have the firmware escape newlines as two-character
`\n` instead of the six-character unicode escape, which drops the worst
case to about 2x.

## Testing and Verification

Add tests around pure logic as soon as modules exist. Hardware-facing code should
be isolated enough that policy and state-machine behavior can be tested without
relay hardware.

For safety-relevant changes, include at least one verification path:

- unit test,
- simulator-style test,
- host-side test,
- or a documented manual hardware check.

## Editing Discipline

- Keep changes focused.
- Do not reformat unrelated code.
- Do not rewrite working modules just to impose a new style.
- Preserve user changes in the worktree.
- Update this file when project conventions become clearer.
