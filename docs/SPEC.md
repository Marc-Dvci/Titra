# Titra specification

Titra is an oxygen controller for preterm babies on low-cost CPAP. It reads each baby's SpO2 once a second, sets the
air-oxygen blender to hold SpO2 in the 91-95% target range, and puts the cot in a defined safe state when a sensor, a
cable or a piece of its own software fails. One QNX 8.0 board serves several cots, and a failure in one cot leaves the
others under control.

## Who it is for

A night-shift nurse in a neonatal unit with several babies on bubble CPAP and one pair of hands. At 03:10 she is
feeding the baby in cot 1 when the oximeter probe slips off the foot of the baby in cot 2. Under manual control, the
oxygen setting in cot 2 stays wherever it was until she notices. Under Titra, cot 2 holds its last setting and raises
an alarm at the cot. If the probe is not back within 30 seconds, it moves to a fallback setting that never sits below
the baby's recent need. Cots 1 and 3 keep their own control throughout.

## The problem, in numbers

- In a randomised crossover trial at two hospitals in southwest Nigeria (September 2022 to September 2023, 49 preterm
  infants on low-cost CPAP), infants spent 30.1% of the time in the SpO2 target range under manual control and 88.1%
  under automated control, an adjusted mean difference of 58.0 percentage points (95% CI 48.0-67.9)
  ([Subhi et al., Lancet Global Health 2025](https://doi.org/10.1016/S2214-109X(24)00458-3)).
- The target range in that trial is SpO2 91-95% while receiving oxygen and 91-100% in room air. Titra uses the same
  definition.
- The algorithm used in that trial is licensed to one manufacturer, and the authors state: "No licensing agreement
  exists pertaining to the low-resource setting." Titra is an independent controller released under the MIT licence.
- On the bench, six commercial controllers differ widely: the maximum rate of sustained FiO2 escalation during
  hypoxaemia ranges from 0.062 to 0.30 per minute, and closed-loop time in target ranges from 51% to 77%
  ([Salverda et al., Arch Dis Child Fetal Neonatal Ed 2026](https://doi.org/10.1136/archdischild-2026-330785)).

## Why a real-time operating system

Titra needs separate protected processes, priority-inheriting message passing and restart on failure, because each
cot must reach its safe FiO2 within a stated budget after any single sensor or software failure, while the other cots
keep their 1 Hz control. The control loop runs once a second, so the case for QNX rests on fault containment and
bounded time to a safe state, which the fault matrix below measures.

| QNX capability | Where Titra uses it |
|---|---|
| Resource managers | `/dev/spo2/N` (oximeter driver), `/dev/valve/N` (blender driver), `/dev/titra/cotN` (cot status) |
| Message passing with priority inheritance | The controller's devctl requests run in each driver at the controller's priority |
| Timer pulses | The controller's 1 Hz tick |
| High Availability Manager | Restarts a driver that dies; restarts a controller that dies, and kills one that misses its heartbeats |
| Security policy | Every process runs under its own type, one set per cot; only the controller of cot N may connect to the drivers of cot N |
| Thread priorities | Control at priority 30, status reads and the nurse station at 10 |

## Architecture

```
 host (bench twin, outside the guest)          QNX 8.0 guest (QEMU x86_64)
 ------------------------------------          ------------------------------------------------------------
 simulated baby + oximeter, cot N  --link-->   spo2d -n N      spo2dN_t     /dev/spo2/N
                                                  ^ devctl (MsgSend, priority inherited)
                                               titractl -n N   titractlN_t  /dev/titra/cotN   1 Hz, priority 30
                                                  v devctl (MsgSend, priority inherited)
 simulated blender, cot N         <--link--    valved -n N     valvedN_t    /dev/valve/N
   own watchdog: hold, then fallback
                                               station         station_t    HTTP nurse view, priority 10
                                               ham             ham_t        restarts and heartbeats
```

Each link is a serial port (`/dev/serN`) or a serial-to-Ethernet converter port (`tcp:HOST:PORT`), the way bedside
devices are often networked. In QEMU all six links of three cots run over TCP to the host, because the emulated PC has
four serial ports.

Each cot has its own three processes. The deterministic core in `core/` (control law, signal quality, frame codecs,
command guard) is plain C11 with no dynamic allocation. The same source files build into the QNX processes and into a
host library that the bench uses for simulation runs, so the controller measured in simulation is the controller that
runs on QNX.

## Safety layers

Every layer follows the same rule: **when its input goes bad, keep the last good output, raise an alarm, and after a
hold time move to the fallback FiO2.**

The fallback FiO2 is the higher of two values:

- the backup FiO2 that the clinician sets for the cot;
- the baseline FiO2, which is the median commanded FiO2 over the last 5 minutes of good input.

Two published findings shaped this rule:

- **Holding comes first.** After a signal dropout, SpO2 usually recovers into a relatively normoxic range, so blind
  FiO2 changes during a dropout are unlikely to help.
- **The fallback never sits below the baseline.** Signal loss is common during prolonged hypoxaemic episodes, and a
  fixed backup FiO2 applied then may prolong the episode
  ([Langanky et al., Eur J Pediatr 2024](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC11192802/)).

| Layer | Bad input | Alarm | Hold, then fallback |
|---|---|---|---|
| Controller (`titractl`) | SpO2 missing or flagged | At once | 30 s from the last valid sample |
| Controller (`titractl`) | SpO2 untrusted (repeated or frozen frames) | At once | Fallback at once; the baseline excludes the untrusted samples |
| Blender driver (`valved`) | No command from the controller for 3 s | At 3 s | 15 s from the last command |
| Blender | No command frame from the driver for 3 s | At 3 s | 15 s from the last frame |

The blender driver also refuses any command outside the cot's FiO2 limits or above its slew limit. In the bench, the
blender layer is the blender twin's own watchdog, outside the guest. It stands for an independent safety circuit in
the device. The clinician sets the backup FiO2 and the FiO2 limits per cot.

A restarted controller begins from the FiO2 that the blender driver reports, so a restart causes no step in FiO2. The
High Availability Manager restarts a process that dies, and it also restarts one that stops sending its heartbeat.

## Signal quality rules

A sample is valid only if all of these hold:

| Rule | Invalid when |
|---|---|
| Fresh | No frame for 3 s |
| Frame intact | Checksum mismatch, malformed field or overlong line |
| Sensor status | The oximeter flags sensor off, no sensor, pulse search, low perfusion or interference |
| Plausible | SpO2 outside 0-100%, or pulse rate outside 30-300 /min |
| Clock advancing | The oximeter timestamp repeats in 3 consecutive frames; those frames are untrusted |
| Not frozen | SpO2, pulse rate and perfusion index identical for 20 consecutive frames; every sample in that run is untrusted |

Low SpO2 is never treated as implausible: a deep desaturation must raise FiO2, not stop the controller.

## Fault matrix

The bench injects each fault and times it on the host clock, outside the guest. The fallback time is measured when
the blender twin receives the fallback FiO2, or applies it through its own watchdog. The cases and their budgets are
committed in `eval/fault_matrix.json` before the code they test.

| ID | Fault | Expected behaviour | Budget |
|---|---|---|---|
| F1 | Oximeter flags sensor off | Hold and alarm, fallback after 30 s | Alarm 2 s, FiO2 unchanged while holding, fallback 32 s |
| F2 | Oximeter cable cut (no frames) | Hold and alarm, fallback 30 s after the last frame | Alarm 5 s, fallback 32 s |
| F3 | Oximeter repeats its last frame | Untrusted after 3 frames, fallback at once | Fallback 5 s |
| F4 | SpO2, pulse and perfusion frozen while the clock advances | Untrusted after 20 frames, fallback at once | Fallback 22 s |
| F5 | Noise bytes between frames, and every third frame corrupted | Bad frames rejected, control continues on the valid ones | No crash, no fallback |
| F6 | Oximeter driver killed | Restarted by HAM, control resumes | Alarm 2 s, resume 10 s, no fallback |
| F7 | Controller killed | HAM restarts the controller, which resumes from the current FiO2; the driver holds the last command meanwhile, and alarms if that takes over 3 s | Resume 10 s, no step at resume |
| F8 | Controller hung (stopped, still alive) | HAM sees the missed heartbeat and restarts it | Alarm 5 s, resume 15 s |
| F9 | Blender driver killed | HAM restarts the driver; the blender holds its last FiO2 meanwhile, and alarms if that takes over 3 s | Resume 10 s |
| F10 | Blender link cut | Blender holds and alarms, fallback 15 s after the last frame; the driver also alarms | Alarm 5 s, fallback 17 s |
| F11 | Out-of-bounds command (FiO2 above the limit, or a step above the slew limit) | Refused by the blender driver and logged | 100% refused |
| F12 | A process of the nurse station's type opens `/dev/valve/N` | Refused by the kernel under the security policy | 100% refused |
| F13 | Every core busy with lower-priority work | Controllers keep their 1 Hz tick | 0 missed ticks |
| F14 | F7 on cot 2 | Cots 1 and 3 keep their 1 Hz commands | 0 missed ticks on cots 1 and 3 |

A missed tick is a command interval longer than 1.5 s.

## Control law

A fixed-gain proportional-integral controller on the error between SpO2 and a 93% setpoint, run once a second:

- separate gains for SpO2 below and above the target range, so hypoxaemia gets a faster response than hyperoxaemia;
- reduced gains inside the target range;
- integration only while the output is not saturated or rate-limited in the same direction (conditional
  integration);
- FiO2 clamped to the cot's limits, with separate per-second limits for increases and decreases, so a frozen reading
  can move FiO2 by at most 20 times the per-second limit before the frozen-value rule stops trusting it;
- on restart, control starts from the current FiO2 reported by the blender driver, so a restart causes no step.

Gains and limits are set in `core/include/titra/config.h` and tuned on the development scenarios only.

## Evaluation

### Step tests (bench protocol of Salverda et al.)

The simulated oximeter holds SpO2 at 80% (hypoxaemia) or 98% (hyperoxaemia) whatever the FiO2. The result is the
maximum rate of sustained FiO2 change, taken as the largest FiO2 change over any 60 s window, in FiO2 per minute.
Salverda et al. measured FiO2 in the respiratory circuit. Titra reports the commanded FiO2 and the blender twin's
modelled circuit FiO2, each labelled.

### Closed loop on a simulated preterm

The simulated baby is described in `docs/MODEL.md`: oxygen delivery through the blender and CPAP circuit, alveolar gas,
shunt and ventilation-perfusion mismatch, a fetal oxygen dissociation curve, circulation delay, intermittent
hypoxaemia episodes, and an oximeter with averaging, quantisation, artefact and sensor-off events.

- Six scenario families: stable, moderate, unstable, worsening, improving and artefact-heavy.
- Development seeds tune the controller. Held-out seeds are fixed in `eval/scenarios.json` now and run once, after
  tuning ends.
- Metrics: time in target (trial definition), time below 85% and below 80%, time above 98% while receiving oxygen,
  FiO2 adjustments per minute, and the largest FiO2 change in any 60 s window.
- Reference controllers on the same scenarios: fixed FiO2, and a rule-based manual-care model described in
  `docs/MODEL.md`.

Simulation figures describe the simulated baby. The published trial and bench figures are quoted beside them as
context, and they come from different patients and simulators.

## Real and simulated

The QNX processes, drivers, resource managers, restart and security policy run on QNX 8.0 in QEMU. The baby, the
oximeter and the blender are simulated on the host.
