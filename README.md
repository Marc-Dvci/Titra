# Titra

**Open oxygen control for preterm babies on low-cost CPAP, built on QNX 8.0: one board holds several babies in the
safe oxygen range and puts each cot in a defined safe state when a sensor, a cable or its own software fails.**

[![Tests](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![QNX 8.0](https://img.shields.io/badge/QNX-8.0-1a73e8)](qnx/)

![The nurse station served by the QNX board: three cots, each with SpO2, FiO2, a 15-minute trend against the 91-95%
target band, and the controller's mode](docs/station.png)

I build software for healthcare in places with few staff and little equipment. Titra fills a gap I found in how
preterm babies on CPAP receive their oxygen.

| Track | Built with | Headline |
|---|---|---|
| Robotics & Medical | QNX SDP 8.0 (QEMU x86_64), C11, Python | 14 of 14 injected faults within budget on QNX with the security policy enforced; 89.4% time in target against 64.8% for fixed FiO2 on held-out simulated babies |

[For judges](#for-judges) · [The problem](#the-problem) · [How it works](#how-it-works) · [QNX](#how-qnx-is-used) ·
[Results](#results) · [Run it](#run-it) · [Claims](#claims)

## For judges

| Criterion | Where it is answered |
|---|---|
| Technological implementation | [How QNX is used](#how-qnx-is-used), [`qnx/`](qnx/), [`core/`](core/), [fault matrix](#fault-matrix-on-qnx) |
| Design | [The nurse station](#the-nurse-station), [safety layers](docs/SPEC.md#safety-layers) |
| Potential impact | [The problem](#the-problem), [closed-loop results](#closed-loop-on-simulated-preterm-babies) |
| Innovation | [Why a real-time OS](#why-a-real-time-os), [per-cot security types](#how-qnx-is-used) |

## The problem

13.4 million babies were born preterm in 2020. Complications of preterm birth are the leading cause of death in
children under five[^3]. Their oxygen must be titrated all day: too little raises mortality, and too much damages the
retina. Retinopathy of prematurity has become an epidemic in sub-Saharan Africa, linked to unmonitored oxygen[^4].
Even in well-staffed units, babies born before 28 weeks were kept in their intended saturation range only 16% to 64%
of the time[^5].

Automation closes that gap. Between September 2022 and September 2023, at two hospitals in southwest Nigeria, preterm
babies on low-cost CPAP spent **30.1%** of the time in the safe oxygen range under manual control and **88.1%** under
automated control. That is an adjusted difference of 58.0 percentage points in a randomised crossover trial of 49
infants[^1]. The algorithm in that trial is licensed to one manufacturer, and its authors write: "No licensing
agreement exists pertaining to the low-resource setting."

The person Titra serves is a night-shift nurse with several babies on bubble CPAP. When a probe slips off one baby's
foot while the nurse is feeding another, that cot holds its oxygen, alarms at once, and after 30 seconds moves to a
fallback that never sits below the baby's recent need. The other cots keep their own control.

## How it works

![Architecture: on the host, the simulated baby, oximeter and blender; in the QNX guest, per cot, the oximeter driver,
the controller and the blender driver, each a separate process with its own security type, plus the nurse station and
the High Availability Manager](docs/architecture.png)

- **The controller** (`titractl`) reads SpO2 once a second through the oximeter driver and sets FiO2 through the
  blender driver: a fixed-gain proportional-integral law with separate gains below, inside and above the 91-95% range,
  per-second rate limits, and hysteresis on the output ([`core/src/control.c`](core/src/control.c)).
- **Signal quality** is decided per frame in the oximeter driver: flags, missing values, implausible pulse rate,
  repeated oximeter clock, and frozen values ([`core/src/quality.c`](core/src/quality.c)).
- **Three safety layers** apply one rule: hold the last good output, alarm, then fall back to the higher of the backup
  FiO2 and the 5-minute baseline. The controller holds for 30 s; the blender driver and the blender each hold for 15 s
  after their input stops ([docs/SPEC.md](docs/SPEC.md#safety-layers)).
- **The core is one set of C11 files** with no dynamic allocation. They build into the QNX processes and into the host
  library the bench simulates with, so the controller evaluated in simulation is the controller running on QNX.

### Why a real-time OS

The loop runs once a second, so jitter is not the argument. The argument is containment and time to a safe state:
each cot's oximeter driver, controller and blender driver are separate protected processes; a controller's requests
run in each driver at the controller's priority; a process that dies is restarted, and a controller that hangs is
killed and restarted; and the kernel refuses any process outside a cot's own controller that tries to reach that
cot's blender. The fault matrix below measures each of these from outside the guest.

## How QNX is used

| QNX capability | What it does in Titra | Where |
|---|---|---|
| Resource managers | `/dev/spo2/N`, `/dev/valve/N` and `/dev/titra/cotN`; `cat` any of them for a JSON status line | [`qnx/common/rm.c`](qnx/common/rm.c) |
| Message passing | The controller's devctl requests run in the drivers at the controller's priority | [`qnx/titractl/titractl.c`](qnx/titractl/titractl.c) |
| Timer pulses | The controller's 1 Hz tick at priority 30 | [`qnx/titractl/titractl.c`](qnx/titractl/titractl.c) |
| High Availability Manager | Restarts a dead driver; restarts a dead controller from its own command line and kills one that misses 5 heartbeats | [`qnx/launch/titra-launch.c`](qnx/launch/titra-launch.c), [`qnx/titractl/titractl.c`](qnx/titractl/titractl.c) |
| Security policy | One set of types per cot (`spo2d2_t`, `valved2_t`, `titractl2_t`); only cot N's controller may connect to cot N's drivers | [`qnx/image/titra.secpol`](qnx/image/titra.secpol), [`qnx/image/check_policy.py`](qnx/image/check_policy.py) |
| mkqnximage | The QEMU image with Titra, HAM and the policy | [`qnx/image/`](qnx/image/) |

### The nurse station

The station (`station`, type `station_t`) runs on the board at priority 10, reads each cot's status through
`/dev/titra/cotN`, keeps an hour of history and serves the view above on port 8080. It shows SpO2, FiO2, a 15-minute
trend against the target band, time in target over the last hour, the controller's mode and every active alarm.

## What it will not do

Titra has been run against simulated babies, oximeters and blenders only, in QEMU. It makes no clinical claim.

## Results

### Fault matrix on QNX

| ID | Fault | Budget | Measured on the host clock | Result |
|---|---|---|---|---|
| F1 | Oximeter flags sensor off | alarm 2 s, fallback 32 s | alarm 0.6 s, fallback 29.2 s | pass |
| F2 | Oximeter cable cut | alarm 5 s, fallback 32 s | alarm 3.6 s, fallback 30.0 s | pass |
| F3 | Oximeter repeats its last frame | fallback 5 s | fallback 1.9 s | pass |
| F4 | SpO2, pulse and perfusion frozen | fallback 22 s | fallback 19.5 s | pass |
| F5 | Noise and corrupt frames on the line | no fallback | no fallback | pass |
| F6 | Oximeter driver killed | alarm 2 s, resume 10 s, no fallback | alarm 0.6 s, resume 1.6 s | pass |
| F7 | Controller killed | resume 10 s, no step | resume 0.9 s, step 0.00 | pass |
| F8 | Controller hung (stopped) | alarm 5 s, resume 15 s | alarm 2.7 s, resume 5.3 s, step 0.00 | pass |
| F9 | Blender driver killed | resume 10 s | resume 1.9 s | pass |
| F10 | Blender link cut | alarm 5 s, fallback 17 s | alarm 3.1 s, fallback 15.1 s | pass |
| F11 | Out-of-bounds commands to the blender driver | 100% refused | 100 of 100 refused | pass |
| F12 | Nurse station's type opens a blender driver | 100% refused | 100 of 100 refused | pass |
| F13 | Every core busy at lower priority, 300 s | 0 missed ticks | 0 missed ticks | pass |
| F14 | Controller of cot 2 killed: cots 1 and 3 watched | 0 missed ticks | step 0.00, 0 missed ticks | pass |

14 of 14 cases pass. Target: QNX 8.0 in QEMU (software emulation), x86_64, 4 CPUs.

Reproduce: build the image (`qnx/image/build.sh ~/titra-image --secpol=secure --policy=$PWD/qnx/image/titra.secpol`),
boot it (`qnx/image/run.sh ~/titra-image`), then run `python -m bench.live --types`. Results:
[`eval/results/faults.json`](eval/results/faults.json).

During the same run on QNX, the two cots left undisturbed kept 85.3% and 85.1% of the time in target over about 45
minutes.

### Closed loop on simulated preterm babies

Four-hour runs, six scenario families, the same babies and oximeters for every controller. Time in target uses the
trial's definition (91-95% on oxygen, 91-100% in room air) on displayed SpO2.

| Controller | Development, 60 runs | Held-out, 60 runs | Below 80%, development |
|---|---|---|---|
| Titra | 89.3% | 89.4% | 0.41% |
| Fixed FiO2 at the baby's starting need | 64.6% | 64.8% | 9.51% |
| Manual-care model | 74.5% | 70.6% | 0.69% |

The gains were tuned on the development seeds only ([`bench/tune.py`](bench/tune.py),
[`eval/tuning_log.jsonl`](eval/tuning_log.jsonl)); the held-out seeds were run once, after tuning ended. In the step
tests of Salverda et al. (SpO2 held at 80% and at 98%), the largest FiO2 change in any minute is 0.090 up and 0.100
down; six commercial devices range from 0.062 to 0.30 up[^2].

> [!IMPORTANT]
> What is real and what is simulated: the QNX processes, drivers, resource managers, restarts and security policy run
> on QNX 8.0 in QEMU. The baby, the oximeter and the blender are simulated on the host
> ([`docs/MODEL.md`](docs/MODEL.md)).

## Run it

### The simulation, no QNX needed

Linux or WSL, with `make`, a C11 compiler and Python 3.12.

```sh
make -C core test                      # build the C core and run its unit tests
python -m venv .venv && . .venv/bin/activate
pip install -r requirements-dev.txt
python -m pytest --cov                 # the bench, and the C core through ctypes
python -m bench steps                  # the step tests
python -m bench sim unstable 0         # one four-hour closed-loop run
python -m bench evaluate development   # every family, seed and controller
```

### On QNX 8.0

QNX SDP 8.0 under the free QNX Everywhere licence, on Ubuntu 22.04 or 24.04 (WSL works), with QEMU.

```sh
source ~/qnx800/qnxsdp-env.sh
make -C qnx                                                         # qcc, x86_64
qnx/image/build.sh ~/titra-image --secpol=secure --policy=$PWD/qnx/image/titra.secpol
qnx/image/run.sh ~/titra-image &                                    # QEMU; ssh on 127.0.0.1:2222, station on :8080
python -m bench.live --types                                        # three simulated cots and the fault matrix
```

Then open http://127.0.0.1:8080 for the nurse station.

## Claims

| Claim | Code | Test |
|---|---|---|
| FiO2 never leaves the cot's limits or the blender driver's slew limit, whatever SpO2 does | [`core/src/control.c`](core/src/control.c), [`core/src/guard.c`](core/src/guard.c) | [`tests/test_chain.py`](tests/test_chain.py) |
| SpO2 lost: FiO2 held, alarm at once, fallback 30 s after the last valid sample | [`core/src/control.c`](core/src/control.c) | [`core/tests/test_control.c`](core/tests/test_control.c), [`tests/test_chain.py`](tests/test_chain.py) |
| The fallback never sits below the backup FiO2 or the 5-minute baseline | [`core/src/control.c`](core/src/control.c) | [`core/tests/test_control.c`](core/tests/test_control.c), [`tests/test_chain.py`](tests/test_chain.py) |
| Repeated or frozen oximeter data is untrusted and its commands leave the baseline | [`core/src/quality.c`](core/src/quality.c) | [`core/tests/test_quality.c`](core/tests/test_quality.c), [`core/tests/test_control.c`](core/tests/test_control.c) |
| A frozen reading moves FiO2 by at most 20 rate steps before it is distrusted | [`core/src/control.c`](core/src/control.c) | [`core/tests/test_control.c`](core/tests/test_control.c) |
| The blender driver refuses commands outside the limits, above the slew limit, or fallbacks below the backup | [`core/src/guard.c`](core/src/guard.c) | [`core/tests/test_guard.c`](core/tests/test_guard.c) |
| The bench and the C core read and write identical frames | [`bench/frames.py`](bench/frames.py), [`core/src/frame.c`](core/src/frame.c) | [`tests/test_frames.py`](tests/test_frames.py) |
| Only each cot's controller may reach that cot's drivers under the committed policy | [`qnx/image/titra.secpol`](qnx/image/titra.secpol) | [`tests/test_policy.py`](tests/test_policy.py) |
| The spec's figures match their published abstracts and the case set | [`eval/reference.json`](eval/reference.json) | [`tests/test_eval_cases.py`](tests/test_eval_cases.py) |

Tests: `make -C core test` runs 79 C tests; `python -m pytest --cov` runs 173 Python tests in a new random order each
time, each within 30 seconds. Coverage of the C core by gcov: 94 to 100% of lines and 82 to 98% of branches per file
(`make -C core coverage`). CI runs both on every push.

## Layout

- `core/` the deterministic C11 core and its unit tests
- `qnx/` the QNX processes, the launcher, the nurse station and the image recipe with its security policy
- `bench/` the bench twin: simulated baby, oximeter and blender, reference controllers, evaluation, tuning, and the live
  bench that runs the fault matrix against the QNX guest
- `eval/` the case set, the tuning log and the results
- `docs/` the specification, the model, the line protocol and the figures
- `tests/` the bench's tests, including the C core through ctypes
- `scripts/` figure and capture tools

## License

[MIT](LICENSE).

[^1]: Subhi R et al. Automated oxygen control for preterm infants receiving continuous positive airway pressure in
    southwest Nigeria: an open-label, randomised, crossover trial. Lancet Glob Health 2025.
    https://doi.org/10.1016/S2214-109X(24)00458-3
[^2]: Salverda HH et al. Neonatal automated oxygen titration algorithms: response characteristics and performance.
    Arch Dis Child Fetal Neonatal Ed 2026. https://doi.org/10.1136/archdischild-2026-330785
[^3]: World Health Organization. Preterm birth, fact sheet. https://www.who.int/news-room/fact-sheets/detail/preterm-birth
[^4]: Rodriguez SH et al. Retinopathy of prematurity in the 21st century and the complex impact of supplemental
    oxygen. J Clin Med 2023. https://doi.org/10.3390/jcm12031228
[^5]: Hagadorn JI et al. Achieved versus intended pulse oximeter saturation in infants born less than 28 weeks'
    gestation: the AVIOx study. Pediatrics 2006. https://doi.org/10.1542/peds.2005-0413
