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
  - `config.c` — validated `qlbattery` parsing and bounded policy helpers
- `tests/behavior.c` — stock-behavior and enhanced-policy regression suite
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

The suite covers PDO parsing and inventory, bounded configuration behavior,
event queue behavior, thermal hysteresis and cycle limits, PPS voltage control,
fixed-charge current ramping, and charger state transitions. Its policy matrices
also encode threshold and boundary behavior recovered from the stock binary.

A passing run prints:

```text
behavior regression suite passed
```

The tests do not emulate the target's kernel drivers. Hardware validation still
requires the actual sysfs interfaces and should be performed under a rollback
guard.

## Bounded charging configuration

This fork makes the four previously dormant `/etc/config/qlbattery` settings
effective. They are read once at startup from the `settings` section:

| Option | Fallback | Effective range | Policy |
| --- | ---: | ---: | --- |
| `max_current_ma` | 5300 mA | 0–5300 mA | Caps every thermal charging-current result; `0` disables buck and pump charging |
| `min_shutdown_mv` | 3400 mV | 3400–3800 mV | Sets the no-adapter low-voltage shutdown threshold |
| `max_pd_vbus_mv` | 9800 mV | 0–9800 mV | Caps every PPS voltage request; values below 6600 mV disable PPS |
| `pd_full_mv` | 4200 mV | 3401–4200 mV | Sets PPS eligibility and the pump-to-buck transition threshold |

Values outside the ranges are clamped toward the safe limit. A malformed
integer is ignored, leaving the prior/default value in effect. Detailed logging
reports both the raw and effective value when `/tmp/quec_battery_log` exists.

These bounds only allow configuration to tighten the recovered stock policy:
current and PPS voltage cannot exceed the stock maxima, shutdown cannot occur
below the stock floor, and PPS cannot remain active above the stock full
threshold.

## Enhanced safety behavior

### PD capability validation

PD capability lines must contain positive voltage and current values, a
non-reversed voltage range, recognized units, and no trailing data other than
the controller's `<-` selection marker. Fractional volts and amps are preserved
as millivolts and milliamps instead of being truncated. Invalid capabilities do
not enter the port inventory and cannot mark a source as PPS-capable.

### PPS contract gating

PDO requests now report matching, write, and refresh failures. The SGM41600
remains disabled until the requested PPS voltage is observed on its VBUS ADC
within 700 mV and the Type-C controller still reports a PPS-capable sink.
Failure at initial negotiation or during a later voltage adjustment disables
the pump and falls back to the port's 5 V buck path. This uses the one-second
settling delay already present in the stock sequence; it adds no new delay.

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

The original behavior remains the reference baseline. Intentional safety
changes in this fork are documented above and covered by behavior regressions
rather than being mixed into the reconstruction silently.
