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

This fork makes the stock charging settings and the optional voltage limit in
`/etc/config/qlbattery` effective. They are read once at startup from the
`settings` section:

| Option | Fallback | Effective range | Policy |
| --- | ---: | ---: | --- |
| `max_current_ma` | 5300 mA | 0–5300 mA | Caps every thermal charging-current result; `0` disables buck and pump charging |
| `min_shutdown_mv` | 3400 mV | 3400–3800 mV | Sets the no-adapter low-voltage shutdown threshold |
| `max_pd_vbus_mv` | 9800 mV | 0–9800 mV | Caps every PPS voltage request; values below 6600 mV disable PPS |
| `pd_full_mv` | 4200 mV | 3401–4200 mV | Sets PPS eligibility and the pump-to-buck transition threshold |
| `charge_limit_mv` | 0 (disabled) | 3800–4200 mV | Applies a non-terminating CV ceiling to buck charging and coordinated SGM41600/PPS voltage regulation |

Values outside the ranges are clamped toward the safe limit. A malformed
integer is ignored, leaving the prior/default value in effect. Detailed logging
reports both the raw and effective value when `/tmp/quec_battery_log` exists.

These bounds only allow configuration to tighten the recovered stock policy:
current and PPS voltage cannot exceed the stock maxima, shutdown cannot occur
below the stock floor, and PPS cannot remain active above the stock full
threshold.

For example, a 4.00 V charge ceiling:

```uci
config battery 'settings'
	option charge_limit_mv '4000'
```

When enabled, the daemon disables the SGM41542S termination bit and programs
its `vreg` to the active minimum of the configured, thermal, and cycle-aging
limits. For PPS charging it programs SGM41600 `BAT_OVP` and `VBAT_REG` with a
200 mV regulation margin, disables the 650 ms regulation timeout, and lowers
the PPS request when battery voltage reaches the same target. SGM41600
regulation is quantized downward to its 25 mV protection steps; the SGM41542S
uses its 10 mV CV steps.

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

### Fixed-current floor

The SGM41542S current ramp is constrained to the inclusive range from zero to
the active policy current. Repeated VBUS droop can reduce charging to the
device-supported zero-current setting, but can no longer generate negative
`ichrg_curr` writes.

### Telemetry validity

Charger, charge-pump, and gauge samples are assembled as complete snapshots;
failed or malformed reads no longer partially update live policy state. A Type-C
power-role read failure produces `UNKNOWN`, never an assumed sink. Buck and pump
monitors require fresh valid charger, battery, and port telemetry and leave the
active charging path on a failed refresh. Consecutive-failure counters reset
only after a complete sample succeeds.

### Watchdog reconnection

The watchdog configuration is checked before opening a socket. Each connection
attempt creates a new close-on-exec socket and completes nonblocking
`EINPROGRESS` with `poll()` and `SO_ERROR`. A lost heartbeat connection is
closed and re-established without reusing the descriptor; ten failed connection
attempts, a charger watchdog fault, or ten consecutive feed/read failures
request a reboot. Signal ownership remains in `main` instead of being replaced
by the worker thread.

### Startup thermal ceiling

Startup now retains the stricter of the temperature voltage ceiling and the
cycle-aging ceiling, using the same minimum-of-limits rule as steady-state
policy. A hot battery therefore starts at 4180 mV rather than being temporarily
overwritten by a 4400 mV low-cycle ceiling.

### Battery-presence normalization

The gauge `present` value is normalized to `unknown`, `absent`, or `present`.
Only the exact value `1` permits charging. A read failure or malformed value
disables the charge paths without being mistaken for battery-free operation.
The exact value `0` retains the stock battery-free power path: the daemon
negotiates an input source and configures input current, but does not enable
battery charging.

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
