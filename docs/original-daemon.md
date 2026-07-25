# Original `quec_battery` daemon behavior

This document describes the behavior recovered from the stock AArch64
`/usr/bin/quec_battery` binary found on a GL.iNet Mudi 7. It is a behavioral
reference, not vendor source documentation. Function names in this repository
are descriptive names assigned during reverse engineering.

The replacement intentionally preserves several oddities of the stock binary.
Those are called out below because they matter when comparing traces or adding
new policy.

## Hardware controlled by the daemon

| Component | Role | Stock sysfs directory |
| --- | --- | --- |
| AW35615-A | USB-C/PD controller for port A | `.../998000.i2c/i2c-3/3-0022/AW35615-A/` |
| AW35615-B | USB-C/PD controller for port B | `.../980000.i2c/i2c-0/0-0022/AW35615-B/` |
| SGM41542S | Conventional buck charger and QC voltage controller | `.../994000.i2c/i2c-2/2-006b/sgm41542s/` |
| SGM41600 | 2:1 switched-capacitor charge pump used for PD PPS | `.../994000.i2c/i2c-2/2-006f/sgm41600/` |
| CW2217 | Fuel gauge and persistent battery-cycle endpoint | `.../994000.i2c/i2c-2/2-0064/` |
| Qualcomm SSUSB | OTG/source-path control | `/sys/devices/platform/soc/a600000.ssusb/` |

The full target-specific paths are defined in `src/qb.h`. The daemon is not a
generic Linux power-supply service: it directly depends on these driver
attributes and on the formatting used by the AW35615 drivers.

## Startup

At startup the daemon:

1. Creates one shared manager containing both ports, both chargers, battery
   telemetry, the mode state, and a 128-entry event queue.
2. Loads `/etc/config/qlbattery` and logs the four values described under
   [Configuration](#configuration).
3. Reads the charger-cycle structure through `libql_sdk.so`, writes the stored
   cycle count to the CW2217 `bat_cycle` attribute, and waits five seconds.
4. Reads the initial battery, PD-port, SGM41542S, and SGM41600 state.
5. Starts the watchdog, gauge, charge-pump, and buck-monitor threads.
6. Selects the initial port/charging mode.
7. Resolves and joins the thermal generic-netlink `event` multicast group and
   discovers the cooling device whose type is `battery-charger-cur`.
8. Starts the queued-event dispatcher, then receives kernel uevents.

Failure to initialize thermal generic netlink prevents the stock daemon from
starting its uevent dispatcher. Once the receiver returns, the stock main loop
continues sleeping rather than exiting.

## Concurrent workers

The stock process is organized around five continuing activities:

- **Uevent receiver:** reads NUL-separated `NETLINK_KOBJECT_UEVENT` messages and
  enqueues recognized Type-C and battery events.
- **Event dispatcher:** serializes resets and mode changes caused by attach,
  detach, role, and battery-presence events.
- **Charge-pump monitor:** re-evaluates mode every three seconds and controls an
  active PPS session every two seconds.
- **Buck monitor:** re-evaluates mode every three seconds, configures fixed PD or
  QC input, and adjusts charge current every two seconds.
- **Gauge/safety monitor:** reads battery state, applies temperature and aging
  limits, handles OTG reserve/HIZ behavior, checks gauge plausibility, and
  performs emergency shutdown decisions.
- **Watchdog monitor:** feeds both charger watchdogs and the external Quectel
  watchdog service.

The charge monitors use interruptible sleeps. A queued hardware event ends the
sleep early so the event dispatcher can reset the charging path before another
control update.

## Port events and mode selection

The receiver recognizes only these AW35615 event values for each port:

- `cc1_in`
- `cc2_in`
- `cc_none`
- `sink`
- `source`

It also recognizes `battery=offline`, `battery=online`, and `battery=dead`.
Duplicate port events are suppressed using the previous event string.

The attachment state selects one of four top-level modes:

| Mode | Condition |
| --- | --- |
| None | Neither port attached |
| Port A | Only AW35615-A attached |
| Port B | Only AW35615-B attached |
| Both | Both ports attached |

For a port acting as a **sink**, the daemon disables that port's source MOSFET
and enables its over-voltage-protection path. For a port acting as a **source**,
it disables the OVP path and enables the source MOSFET/OTG path. Role-change
events update the cached MOS/OVP state before the charging reset.

A reset returns both PD controllers and the QC controller toward 5 V, disables
the buck and charge pump, clears working-state fields, and causes mode selection
to run again. A battery-offline event has a special reset path intended to avoid
interrupting a buck path that was keeping an otherwise dead system alive.

When both ports are attached, the source/sink combination determines which
power paths may remain active. If both attached ports are sinks, the daemon
chooses one charging input rather than combining their power.

## PD capability discovery

Each AW35615 exposes a textual `pdo_set` file. The daemon parses:

- fixed 5 V, 9 V, and 12 V PDOs;
- one or more PPS/APDO lines;
- the currently selected fixed PDO marker.

It stores the advertised current and PDO number for each fixed voltage, plus the
PPS minimum voltage, maximum voltage, and current. The original parser uses
`atoi()` on PPS voltage tokens, so a minimum such as 3.3 V is intentionally
stored as 3 V. A malformed line beginning with `Pps` is also accepted with any
missing fields left at zero.

A fixed PDO request writes `<pdo-number>  <current-mA>` to `pdo_set`. A PPS
request writes `<voltage-mV>  <current-mA>`. The double space is intentional and
matches the stock payload.

Port B may also be connected to a QC source. The daemon probes 12 V first, then
9 V, measures SGM41542S VBUS, records the highest successful level, and returns
the source to 5 V before normal charger setup.

## Choosing buck charging or PPS

The SGM41600 PPS path is permitted only when all of the following are true:

- pump battery voltage is above 3400 mV;
- pump battery voltage is below the internal PPS-full threshold of 4200 mV;
- the selected input advertises PPS;
- temperature state is Normal, Normal-high, or Warm;
- the pump has not latched its repeated-zero-input-current error.

Otherwise the daemon uses the SGM41542S buck path if temperature and role permit
charging.

With two sink inputs, port A wins if it advertises PPS. It can also win when its
maximum fixed voltage is greater than both port B's fixed capability and the
QC voltage found through port B. Otherwise port B is selected.

## SGM41600 PPS control

A PPS session starts near `2.20 × VBAT`. The initial requested current is derived
from the active thermal charge-current limit; subsequent requests use 2650 mA.
The charge pump is enabled with `charge_en=2`.

Every control pass applies the following stock policy:

- Raise requested voltage by 100 mV when battery current is at least 300 mA
  below the active current limit and battery voltage has not exceeded the PPS
  full threshold.
- Do not raise beyond approximately `2.35 × VBAT`.
- Lower requested voltage by 50 mV when battery current exceeds the limit or
  battery voltage exceeds 4300 mV.
- Do not lower past approximately `2.02 × VBAT`.
- Clamp every resulting request to 6600–9800 mV.

Eleven consecutive samples with zero SGM41600 input current latch a pump error.
The monitor then disables the pump and converts the selected input back to
5 V buck charging. It also leaves PPS when battery voltage reaches 4200 mV with
battery current at or below 2000 mA, temperature leaves the PPS window, battery
voltage falls below 3401 mV, or PPS capability disappears.

## SGM41542S fixed/QC charging

The fixed-current ramp starts at 300 mA. Every control pass:

- subtracts 200 mA when measured VBUS is below 4300 mV;
- adds 200 mA when measured VBUS is at least 4801 mV and current is below the
  active thermal limit;
- caps only the upper end at the active thermal limit.

There is deliberately no lower clamp. This means the retained ramp value can
become negative if low VBUS persists; the reimplementation preserves that stock
behavior.

Input-current/VINDPM settings depend on measured VBUS:

| Measured VBUS | Input-current limit | VINDPM |
| --- | ---: | ---: |
| 4601–7000 mV | 2.0 A | 3.9 V |
| 7001–10500 mV | 2.0 A | 7.5 V |
| ≥10501 mV | 1.5 A | 10.5 V |

When reported battery charge current remains zero for eleven checks while the
charger should be active, below full voltage, and outside the hot states, the
daemon toggles `charge_en` off and on.

The buck monitor can hand an active input back to the charge pump if battery
voltage and temperature later enter the PPS window.

## Temperature and cycle policy

Temperatures are in tenths of a degree Celsius. Runtime control uses hysteresis
bands in which the previous state and limits are retained:

| Temperature | Result | Full voltage | Charge current |
| --- | --- | ---: | ---: |
| ≤0 | Cold | 4400 mV before cycle cap | 0 mA |
| 1–29 | Hold previous state | unchanged | unchanged |
| 30–149 | Cool | 4400 mV before cycle cap | 1325 mA |
| 150–179 | Hold previous state | unchanged | unchanged |
| 180–349 | Normal | 4400 mV before cycle cap | 5300 mA |
| 350–379 | Hold previous state | unchanged | unchanged |
| 380–419 | Normal-high | 4400 mV before cycle cap | 3710 mA |
| 420–449 | Hold previous state | unchanged | unchanged |
| 450–469 | Warm | 4180 mV | 2650 mA |
| 470–499 | Hold previous state | unchanged | unchanged |
| 500–569 | Hot | 4180 mV | 1325 mA |
| 570–599 | Hold previous state | unchanged | unchanged |
| ≥600 | Overheat | 4180 mV | 0 mA |

When SGM41542S battery voltage is below 3001 mV, nonzero thermal current limits
are reduced to 275 mA.

Battery cycle count caps full voltage and defines the hot-state capacity
threshold:

| Cycle count | Full-voltage cap | `v42_capacity` |
| ---: | ---: | ---: |
| 0–100 | 4400 mV | 86% |
| 101–200 | 4350 mV | 88% |
| 201–500 | 4300 mV | 92% |
| ≥501 | 4250 mV | 98% |

The thermal cooling-device state overrides this table:

- state 1 forces Hot, 4180 mV, and either 1060 mA or 275 mA depending on battery
  voltage;
- state 2 forces Overheat and zero charge current.

At process startup, the original initializes temperature with broader bands and
then unconditionally overwrites full voltage with the cycle-based cap. This can
produce a temporarily surprising full-voltage value in a hot startup state.

## Battery, HIZ, OTG, and safety handling

The gauge monitor performs several independent policies:

- Reads `gl_otg.typec1.threshold` and mirrors it to SGM41542S `set_cap`.
- Below that capacity reserve, disables both source MOSFETs, records
  `/tmp/power_limit_state`, and reselects charging mode. When capacity recovers,
  it clears the limit and restarts `usb_otg_manage`.
- Prevents source MOS operation at battery temperatures at or below 10.0 °C or
  at or above 50.0 °C. This broad cutoff is distinct from the later emergency
  poweroff thresholds.
- Disables both source paths at zero capacity when no sink adapter is attached.
- Manages SGM41542S HIZ around the Hot-state voltage/capacity/current boundary.
- Powers the unit off after more than ten danger samples when there is no sink
  adapter and charger battery voltage is below 3400 mV, or when battery
  temperature is at or below -10.0 °C or at or above 58.0 °C.

When battery current is between -149 and +149 mA, at most once per hour, it
compares CW2217 data with SGM41542S battery ADC data. It asks the CW2217 driver
to reset if voltage differs by at least 1201 mV or if capacity differs from the
stock OCV-derived estimate by more than 20 percentage points.

The daemon synchronizes CW2217 cycle count back into Quectel's persistent
charger configuration every 43,200 elapsed seconds when the gauge value is
newer. The SDK structure is 16 bytes with the cycle count at offset 4, and the
setter receives that structure by value.

A `battery=dead` event performs up to ten half-second recovery attempts by
writing a 150 mA charge current and re-enabling the SGM41542S before dispatching
the event.

## Watchdog behavior

`/etc/ChargeIC_wdt` controls watchdog operation. When enabled, the daemon:

1. Configures the SGM41542S and SGM41600 watchdog attributes.
2. Connects a nonblocking Unix socket to `/tmp/wdt_server.sock`.
3. Sends `HEARTBEAT` every five seconds.
4. Reads both charger watchdog fault states.
5. Feeds the SGM41542S watchdog.

Ten connection attempts are made at five-second intervals. A connection timeout
requests a reboot. Fatal heartbeat, charger-fault, or repeated feed failures
also disable Qualcomm hypervisor user-petting before rebooting. When watchdogs
are disabled, the daemon stops `ql_wdt_service`, writes the charger disable
values, and disables hypervisor user-petting.

## Configuration

### `/etc/config/qlbattery`

The stock daemon reads the `settings` section and stores/logs:

| Option | Stock default | Observed runtime use |
| --- | ---: | --- |
| `max_current_ma` | 5300 | Loaded and logged; charging policy still uses hard-coded limits |
| `min_shutdown_mv` | 3400 | Loaded and logged; shutdown comparison remains hard-coded |
| `max_pd_vbus_mv` | 9800 | Loaded and logged; PPS clamp remains hard-coded at 9800 mV |
| `pd_full_mv` | 4050 | Loaded and logged; active PPS-full threshold is separately initialized to 4200 mV |

These ineffective tunables are a confirmed property of the examined stock
binary, not an omission in the reconstruction.

### Other configuration and state

- `/etc/config/gl_otg`, section `typec1`, option `threshold`: active capacity
  reserve for USB-C source/OTG operation.
- `/etc/ChargeIC_wdt`, `watchdog_enable = <0|1>`: controls watchdog handling.
- `/tmp/power_limit_state`: records whether the capacity reserve has disabled
  source paths.
- `/tmp/quec_battery_log`: its presence enables detailed syslog output.

## Preserved stock quirks

Important intentional quirks include:

- fractional PPS voltages are truncated by `atoi()`;
- malformed `Pps` lines can be accepted with zero fields;
- fixed-charge current has no lower clamp;
- several similarly named current fields have different units and roles;
- the four `qlbattery` options are loaded but do not drive the active policy;
- startup temperature initialization overwrites its own full-voltage result
  with the cycle cap;
- some port-A and port-B MOS/OVP write sequences are asymmetric;
- raw battery `present` values are retained because different stock call sites
  test either exactly `1` or merely nonzero;
- the watchdog's enabled path retains the stock reconnect/control-flow behavior.

These should not be “cleaned up” in a fidelity change. New product policy should
be introduced explicitly and covered by behavior tests.
