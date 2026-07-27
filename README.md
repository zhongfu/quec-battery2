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
| `charge_limit_mv` | 0 (disabled) | 0, or 3800–4200 mV | Applies a terminating CV ceiling with coordinated SGM41600/PPS bulk charging and SGM41542S completion |
| `charge_limit_percent` | 0 (disabled) | 0 (disabled), or 1–100% | Holds battery charge current at zero after three samples at the limit; resumes after three samples at least three percentage points lower |

Configuration values are base-10 integers in the units shown above. A malformed
integer is ignored, leaving the prior/default value in effect. Detailed logging
reports both the raw and effective value when `/tmp/quec_battery_log` exists.
The exact normalization rules for the two optional charge limits are:

| Raw value | Effective `charge_limit_mv` |
| ---: | ---: |
| less than 0 | 0 (disabled) |
| 0 | 0 (disabled) |
| 1–3799 mV | 3800 mV |
| 3800–4200 mV | unchanged |
| greater than 4200 mV | 4200 mV |

| Raw value | Effective `charge_limit_percent` |
| ---: | ---: |
| less than 0 | 0 (disabled) |
| 0 | 0 (disabled) |
| 1–100% | unchanged |
| greater than 100% | 100% |

These bounds only allow configuration to tighten the recovered stock policy:
current and PPS voltage cannot exceed the stock maxima, shutdown cannot occur
below the stock floor, and PPS cannot remain active above the stock full
threshold.

The 3800 mV active floor is retained from the SGM41600 charge-pump policy.
The daemon places the pump's hardware `VBAT_REG` point 100 mV above the
effective software target, capped at 4300 mV. At the 3800 mV floor this yields
a 3900 mV regulation point and a 4000 mV `BAT_OVP` threshold. Pump register
targets are rounded downward to 25 mV steps.
The SGM41542S buck charger itself supports lower `VREG` values, down to
3500 mV in 10 mV steps, but this daemon does not interpret a sub-3800 mV
setting as a request for buck-only charging. Any positive value below 3800 mV
is clamped to 3800 mV instead.

With an active voltage limit, mode selection requires the battery to be at
least 100 mV below the effective target before the charge pump is eligible.
Inside that final 100 mV window the buck path is selected directly. This avoids
starting the SGM41600 in its `VBAT_REG` region, where the rapidly falling input
current can trigger `IBUS_UCP` and reset divider mode.

When PPS charging begins below that window, the daemon starts at 2.2 times the
measured battery voltage. This preserves the recovered stock startup ratio and
provides enough voltage headroom for input current to cross the SGM41600's
programmed under-current threshold despite cable and board-path voltage drop.
After divider mode is confirmed, the daemon retains the recovered 100 mV
approach steps until the battery enters the final 100 mV around an active
voltage target. Inside the window it uses protocol-native 20 mV PPS steps.
It does not raise PPS while pump `vbat_adc` is in the asymmetric dead zone from
25 mV below the target through the target, and lowers PPS whenever that ADC is
above the target. Requests remain capped at 2.2 times the effective
battery-voltage target. A battery voltage at least 100 mV above the target
restores a 100 mV retreat.

For an active voltage limit, two consecutive pump samples at or above 25 mV
below the target and at or below 1500 mA initiate a clean handoff to the
SGM41542S. The first qualifying sample suppresses only that pass's PPS
adjustment; it does not create a persistent voltage hold. A reading 50 mV above
the target initiates handoff immediately. The daemon pre-programs the buck
voltage and termination state, disables the pump, and verifies that its
converter is off before enabling the SGM41542S at a conservative 300 mA. It
then requests the highest advertised fixed PDO directly—12 V, 9 V, or 5 V—
without an intermediate 5 V reset and continues the normal buck-current ramp
toward the active thermal/current ceiling. The SGM41542S then transitions from
constant current to constant voltage at its programmed `VREG` and tapers charge
current in hardware. The selected port is latched to buck charging
until that cable detaches. This avoids overlapping the two battery-charging
converters while minimizing the interruption and prevents prolonged SGM41600
`VBAT_REG` operation, which can increase the external OVPFET voltage drop
enough to trip `VDRP_OVP`. If pump shutdown cannot be confirmed, the buck
remains disabled and PD is returned to 5 V. Without an active configured
voltage limit, handoff uses the effective full-voltage target, capped at the
stock 4200 mV boundary, together with the recovered 2000 mA battery-current
threshold. Pump startup, telemetry, capability, and protection failures use
the conservative 5 V electrical fallback without being classified as a
completed charge handoff.

For example, a 4.00 V ceiling combined with an 80% capacity limit:

```uci
config quec_battery_configs 'settings'
	option charge_limit_mv '4000'
	option charge_limit_percent '80'
```

The daemon explicitly enables SGM41542S hardware termination whenever it
programs the buck voltage, including after a limited-PPS handoff. The hardware
termination-current setting is otherwise left intact; its reset value is
180 mA. The daemon programs constant-charge voltage through the readable vendor
`vreg` attribute, using the active minimum of the configured, thermal, and
cycle-aging limits. The standard power-supply `constant_charge_voltage`
attribute writes the same hardware register on this target, but its read method
always reports zero. The daemon therefore verifies the setting through `vreg`
and reapplies it if the charger register drifts or resets. For PPS charging it
programs SGM41600 `VBAT_REG` 100 mV above the software target, capped at
4300 mV, and normally places `BAT_OVP` another 50 mV higher. It also disables
the 650 ms regulation timeout. SGM41600 regulation is quantized downward to its
25 mV protection steps; the SGM41542S uses its 10 mV CV steps.

PPS feedback, fine adjustment, and handoff decisions use the SGM41600
`vbat_adc` reading. The `vbat:` field in the gauge-monitor log is instead the
SGM41542S ADC reading, while the CW2217 reports a third value through
`voltage_now`. Those ADCs can have different fixed offsets, so the logged
SGM41542S value alone does not show that PPS exceeded the configured limit.

The capacity limit is evaluated only from complete, range-checked gauge
snapshots. Three consecutive samples are required both to enter the hold and
to resume, with a fixed three-percentage-point hysteresis. Entering the hold
disables the charge pump, returns PD to 5 V, and leaves the SGM41542S input
power path available for the system while battery charging is disabled. The
voltage and capacity limits compose independently; whichever becomes
restrictive first controls battery current.

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
