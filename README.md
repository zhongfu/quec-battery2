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
- `packaging/` — OpenWrt/opkg package sources
- `.github/workflows/release.yml` — build and release automation
- `docs/` — reference documentation:
  - `original-daemon.md` — recovered stock behavior, thresholds, and quirks
  - `charging-policy.md` — fork charging limits and safety policy
  - `packaging.md` — package layout and opkg behavior
- `Makefile` — build, test, package, and clean targets

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

## Package (OpenWrt / opkg)

Build the package:

```sh
make clean
make CC=aarch64-linux-musl-gcc
make ipk
```

The build writes `dist/quec-battery2_<version>_<arch>.ipk`. The Mudi 7
architecture is `aarch64_cortex-a53`.

Install or remove the package:

```sh
opkg install dist/quec-battery2_<version>_aarch64_cortex-a53.ipk
opkg remove quec-battery2
```

The package installs `/usr/bin/quec_battery2`. On install, it moves the stock
daemon to `/usr/bin/quec_battery.stock` and points `/usr/bin/quec_battery` at
the new binary. On removal, it puts the stock daemon back. The vendor package,
init script, and configuration stay in place.

For the package layout and opkg behavior, see
[`docs/packaging.md`](docs/packaging.md).

On a `v*` tag, `.github/workflows/release.yml` tests the code, builds the
package, and publishes the binary and package to the GitHub release.

## Configuration

The daemon reads `/etc/config/qlbattery` once at startup. The `settings` section
accepts these options:

| Option | Default | Allowed range |
| --- | ---: | --- |
| `max_current_ma` | 5300 mA | 0–5300 mA |
| `min_shutdown_mv` | 3400 mV | 3400–3800 mV |
| `max_pd_vbus_mv` | 9800 mV | 0–9800 mV |
| `charge_limit_mv` | 0 (stock policy) | 0, or 3800–4400 mV |
| `charge_limit_percent` | 0 (off) | 0, or 1–100% |

Values are base-10 integers. A value outside the range is clamped or ignored.
`0` for the two charge limits keeps the stock policy.

Example: stop charging at 4.00 V or 80%:

```uci
config quec_battery_configs 'settings'
	option charge_limit_mv '4000'
	option charge_limit_percent '80'
```

For the exact rules and the safety policy, see
[`docs/charging-policy.md`](docs/charging-policy.md).

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
changes in this fork are documented in
[`docs/charging-policy.md`](docs/charging-policy.md) and covered by behavior
regressions rather than being mixed into the reconstruction silently.
