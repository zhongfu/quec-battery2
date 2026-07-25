# `quec_battery` reimplementation

*Slop disclaimer: practically all work here was done by an LLM (GPT-5.6-Sol),
though functionality has been briefly verified on a real Mudi 7*

Behavioral C reimplementation of the stock `quec_battery` charging daemon from
a GL.iNet Mudi 7.

The target uses two AW35615 USB-C/PD controllers, an SGM41542S buck charger, an
SGM41600 2:1 charge pump for PD PPS charging, and a CW2217 fuel gauge. The source
was reconstructed from the stripped AArch64 binary and checked against the
original daemon's control flow, constants, sysfs payloads, and behavior on the
device.

This is target-specific reverse-engineered software, not vendor source. Do not
install it on hardware without a recovery path and automatic rollback.

## Repository contents

- `src/` — daemon implementation
  - `main.c` — startup and worker lifecycle
  - `core.c` — charging modes, resets, QC probing, and control policies
  - `pd.c` — AW35615 capability parsing and PD/PPS requests
  - `chips.c` — charger, charge-pump, battery, MOS, OVP, and OTG sysfs access
  - `monitors.c` — buck and charge-pump worker loops
  - `thermal.c` — battery thermal, aging, HIZ, OTG reserve, and gauge policy
  - `events.c` — kernel uevent receiver and event dispatcher
  - `thermal_events.c` — thermal generic-netlink listener
  - `watchdog.c` — charger and system watchdog handling
  - `config.c` — stock `qlbattery` configuration parsing
- `tests/behavior.c` — deterministic stock-behavior regression suite
- `docs/original-daemon.md` — detailed description of the recovered stock
  daemon behavior, thresholds, state machine, configuration, and quirks
- `Makefile` — native build, test, and clean targets

## Build

A native build is useful for compiler checks:

```sh
make clean
make
```

For the target, use an AArch64 musl compiler:

```sh
make clean
make CC=aarch64-linux-musl-gcc
file quec_battery
```

The resulting target executable expects `/lib/ld-musl-aarch64.so.1`. It uses
POSIX threads and `libdl`; `libql_sdk.so` is loaded at runtime for persistent
battery-cycle data.

## Test

Run the host-side behavior suite with:

```sh
make clean
make test
```

The suite covers PDO parsing and inventory, configuration loading, event queue
behavior, thermal hysteresis and cycle limits, PPS voltage control, fixed-charge
current ramping, and charger state transitions. Its policy matrices encode
threshold and boundary behavior recovered from the stock binary.

A passing run prints:

```text
stock behavior regression suite passed
```

The tests do not emulate the target's kernel drivers. Hardware validation still
requires the actual sysfs interfaces and should be performed under a rollback
guard.

## Behavior reference

See [`docs/original-daemon.md`](docs/original-daemon.md) before changing charging
or safety policy. It documents:

- port attachment, sink/source, MOS, and OVP state transitions;
- fixed PD, QC, and PPS source selection;
- SGM41600 PPS voltage/current control;
- SGM41542S fixed-current ramping;
- temperature hysteresis and cycle-aging limits;
- battery, HIZ, OTG-reserve, gauge-reset, and shutdown behavior;
- watchdog and battery-cycle persistence;
- configuration values that the stock binary reads but does not actually use.

The implementation deliberately retains confirmed stock quirks so that policy
changes can be made from a faithful baseline rather than silently changing
behavior during reconstruction.
