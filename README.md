# Titra

**Open oxygen control for preterm babies on low-cost CPAP, built on QNX 8.0: one board holds several babies in the safe
oxygen range, puts each cot in a defined safe state when a sensor, a cable or its own software fails, and runs a local
language model beside them that the kernel keeps away from every valve.**

[![Tests](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![QNX 8.0](https://img.shields.io/badge/QNX-8.0-1a73e8)](qnx/)
[![Faults on QNX: 23 of 23 within budget](https://img.shields.io/badge/faults%20on%20QNX-23%20of%2023-188038)](#fault-matrix-on-qnx)
[![Tests: 173 C and 297 Python](https://img.shields.io/badge/tests-173%20C%20%C2%B7%20297%20Python-188038)](#claims)
[![Bench coverage: 96%](https://img.shields.io/badge/bench%20coverage-96%25-188038)](pyproject.toml)

![The nurse station served by the QNX board: three cots, each with SpO2, FiO2, a 15-minute trend against the 91-95%
target band and the controller's mode, and under each cot the advisor's action and sentence with its flags and
record](docs/station.png)

I build software for healthcare, mostly for places with few staff and little equipment. Titra fills a gap I found in
how preterm babies on CPAP receive their oxygen.

| Track | Built with | Headline |
|---|---|---|
| Robotics & Medical | QNX SDP 8.0 (QEMU x86_64 with hardware acceleration), C11, llama.cpp, NVIDIA Nemotron Labs Dory 1.0 4B, Python | 23 of 23 injected faults within budget on QNX with the security policy enforced; 0 missed ticks while the model generates flat out for 10 minutes; 89.4% time in target against 64.8% for fixed FiO2 on held-out simulated babies |

[For judges](#for-judges) · [See it work](#see-it-work) · [The problem](#the-problem) · [How it works](#how-it-works) ·
[QNX](#how-qnx-is-used) · [The AI](#the-ai-on-the-board) · [Results](#results) · [Run it](#run-it) · [Claims](#claims)

> [!TIP]
> The controller runs without QNX: `make -C core test`, then `pip install -r requirements-dev.txt` and
> `python -m bench sim unstable 0` simulate four hours of an unstable baby in under a second. Full steps:
> [Run it](#run-it).

## For judges

| Criterion | Where it is answered | Code at the submitted tag |
|---|---|---|
| Technological implementation | [How QNX is used](#how-qnx-is-used), [fault matrix](#fault-matrix-on-qnx) | [resource manager](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/common/rm.c#L91-L116), [1 Hz pulse and devctl to both drivers](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/titractl/titractl.c#L189-L228), [HAM restarts](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/launch/titra-launch.c#L78-L90), [HAM heartbeat](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/titractl/titractl.c#L168-L178), [/dev/llm on a fixed-priority channel](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/llm/llm.cpp#L1034-L1036) |
| Design | [See it work](#see-it-work), [the nurse station](#the-nurse-station), [safety layers](docs/SPEC.md#safety-layers), [example answer](examples/) | [control step](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/core/src/control.c#L153-L237), [station page](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/station/index.html), [validator](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/core/src/advice.c#L595-L634) |
| Potential impact | [The problem](#the-problem), [closed-loop results](#closed-loop-on-simulated-preterm-babies) | [held-out results](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/eval/results/held_out.json), [fault results](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/eval/results/faults.json) |
| Innovation & AI creativity | [The AI on the board](#the-ai-on-the-board), [advisor](docs/ADVISOR.md), [system card](docs/SYSTEM_CARD.md) | [loop count and earliest deadline per token](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/llm/llm.cpp#L388-L434), [the model's policy](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/image/titra.secpol#L86-L105), [the model's self-attack](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/llm/llm.cpp#L863-L994), [AVX2 kernels](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/qnx/llm/kernels/q5k_avx2.inc), [fault runner](https://github.com/Marc-Dvci/Titra/blob/v1.0.0/bench/live.py#L238-L398) |

## See it work

![Cot 2's probe comes off: its SpO2 shows two dashes, the cot holds FiO2 and raises the alarm, and 30 seconds later it
moves to the fallback FiO2 of 0.30, while cots 1 and 3 stay in control](docs/probe-fault.gif)

Cot 2's probe comes off during a fault run. The cot holds its oxygen and alarms within a second, and about 30
seconds after the probe came off it moves to the fallback, while cots 1 and 3 keep their own control. These are the nurse
station's own frames, recorded from the QNX board every 2 seconds and played at 4x speed.

![The model's process hangs: the line under the advice says the model is unavailable, and every cot's SpO2 and FiO2 carry
on until it is back](docs/model-hung.gif)

The model's process hangs while the cots run. The page says the model is unavailable, the High Availability Manager kills it
when its heartbeats stop and restarts it from the weights in shared memory, and no cot misses a tick.

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
fallback that never sits below the baby's recent need. The other cots keep their own control. When the nurse looks up,
the board says which cot to go to first, and why.

## How it works

![Architecture: on the host, the simulated baby, oximeter and blender. In the QNX guest, on CPU 0 at priority 30, each
cot's oximeter driver, controller and blender driver, each a separate process with its own security type. On CPUs 1-3,
the advisor and the language model at low priority, with the weights in sealed shared memory. The nurse station reads
both. The High Availability Manager supervises them all. The policy refuses the station, the advisor and the model at
every blender driver](docs/architecture.png)

- **The controller** (`titractl`) reads SpO2 once a second through the oximeter driver and sets FiO2 through the
  blender driver: a fixed-gain proportional-integral law with separate gains below, inside and above the 91-95% range,
  per-second rate limits, and hysteresis on the output ([`core/src/control.c`](core/src/control.c)).
- **Signal quality** is decided per frame in the oximeter driver: flags, missing values, implausible pulse rate,
  repeated oximeter clock, and frozen values ([`core/src/quality.c`](core/src/quality.c)).
- **Three safety layers** apply one rule: hold the last good output, alarm, then fall back to the higher of the backup
  FiO2 and the 5-minute baseline. The controller holds for 30 s; the blender driver and the blender each hold for 15 s
  after their input stops ([docs/SPEC.md](docs/SPEC.md#safety-layers)).
- **The advisor** reads each controller's status, computes facts and flags in C, and asks the model for one sentence per
  flagged cot ([docs/ADVISOR.md](docs/ADVISOR.md)).
- **The core is one set of C11 files** with no dynamic allocation. They build into the QNX processes and into the host
  library the bench simulates with, so the controller evaluated in simulation is the controller running on QNX.

### Why a real-time OS

The loop runs once a second, so jitter is not the argument. The argument is containment and time to a safe state:
each cot's oximeter driver, controller and blender driver are separate protected processes; a controller's requests
run in each driver at the controller's priority; a process that dies is restarted, and a controller that hangs is
killed and restarted; and the kernel refuses any process outside a cot's own controller that tries to reach that
cot's blender.

The same properties let a 3.67 GB language model share the board with the controllers. The model has its own CPUs, a
priority no client can lift, no path to any driver, and a supervisor that restarts it in seconds. The fault matrix below
measures each of these from outside the guest.

## How QNX is used

| QNX capability | What it does in Titra | Where |
|---|---|---|
| Resource managers | `/dev/spo2/N`, `/dev/valve/N`, `/dev/titra/cotN`, `/dev/titra/advice` and `/dev/llm`; `cat` any of them for a JSON status line | [`qnx/common/rm.c`](qnx/common/rm.c), [`qnx/llm/llm.cpp`](qnx/llm/llm.cpp) |
| Message passing | The controller's devctl requests run in the drivers at the controller's priority. The model's channel is the opposite: `_NTO_CHF_FIXED_PRIORITY`, so a client at priority 20 leaves it at 6 | [`titractl.c`](qnx/titractl/titractl.c), [`llm.cpp`](qnx/llm/llm.cpp) |
| Timer pulses | The controller's 1 Hz tick at priority 30 | [`titractl.c`](qnx/titractl/titractl.c) |
| High Availability Manager | Restarts a dead driver; restarts a dead controller or model from its own command line; kills one that misses its heartbeats. The model's heartbeat is inference progress, not liveness | [`titra-launch.c`](qnx/launch/titra-launch.c), [`titractl.c`](qnx/titractl/titractl.c), [`llm.cpp`](qnx/llm/llm.cpp) |
| Security policy | One set of types per cot (`spo2d2_t`, `valved2_t`, `titractl2_t`), plus `station_t`, `advisor_t`, `llm_t`; only cot N's controller may connect to cot N's drivers | [`titra.secpol`](qnx/image/titra.secpol), [`check_policy.py`](qnx/image/check_policy.py) |
| Partitioning and startup | Controllers on CPU 0; the advice cluster on CPUs 1-3 (`startup-x86 -c advice:0xe`); `procnto -P 22` caps every process without the priority ability below the controllers | [`qnx/image/build.sh`](qnx/image/build.sh) |
| Shared memory | The weights: filled once at boot, SHA-256 checked, sealed and read-only (`SHMCTL_SEAL`), physically contiguous, and mapped by the model, so a restart does not reload 3.67 GB | [`llmstore.c`](qnx/llm/llmstore.c) |
| qcc and mkqnximage | llama.cpp and Dory's looped graph built for QNX 8.0 with a CMake toolchain; the image holds Titra, HAM and the policy | [`qnx/llm/`](qnx/llm/), [`qnx/image/`](qnx/image/) |

### The nurse station

The station (`station`, type `station_t`) runs on the board at priority 10, reads each cot's status through
`/dev/titra/cotN` and the advice through `/dev/titra/advice`, keeps an hour of history and serves the view above on port
8080. It shows SpO2, FiO2, a 15-minute trend against the target band, time in target over the last hour, the controller's
mode, every active alarm, and for each cot the advisor's action, sentence, forecast, record and the nurse's note. A nurse
can write a note and ask for the shift handover. The incident notes for the biomedical engineer sit below.

## The AI on the board

Two models run on the QNX board, and C decides what either can say. I found no public port of llama.cpp to QNX 8.0; this
one is in [`qnx/llm/`](qnx/llm/).

| Part | What it is | What it decides | Held-out result |
|---|---|---|---|
| Facts and flags | C over the controller's status ([`facts.c`](core/src/facts.c)) | Every number, every flag, the order of the tiers | Recall 90% to 100% per flag on 90 windows |
| Forecast | 60 trees of depth 2, trained on simulated babies, plain C ([`risk.c`](core/src/risk.c)) | The chance of a dip below 85% within ten minutes | AUROC 0.704 against 0.668 for the best rule; 0.21 µs mean on the board |
| Language model | NVIDIA Nemotron Labs Dory 1.0 4B, Q5_K_M, as published, as a QNX resource manager ([`llm.cpp`](qnx/llm/llm.cpp)) | The sentence about a flagged cot, its action from the allowed list, the order within a tier | 53 of 54 answers valid at one loop; the first cot equals the rules' first in 97.7% |
| Validator | C ([`advice.c`](core/src/advice.c)), trusting nothing from the grammar | Whether an answer is shown at all | CI re-validates every recorded answer |

**The boundary.** The model writes only from a vocabulary composed per cot from that cot's flags, under a grammar composed
per request, with no digit in it. A cot with no flag gets the rules' own sentence. If the model is unavailable, late or
refused, the page shows the flags. There is no word for a dose or a setting, and no nurse's note can add one
([example answer, start to finish](examples/)).

**A looped model under real-time deadlines.** Dory's recurrent block runs one to four times per token. At every token
the server picks the session with the earliest deadline, then the largest loop count that still meets it, from cost
measured live on this board ([`loops.c`](core/src/loops.c)). An urgent request overtakes a long one at the next token.

**Kernel work.** Q5_K weights had no x86 path in ggml's repacked layout. New AVX2 kernels for the 8x8 layout
([`q5k_avx2.inc`](qnx/llm/kernels/q5k_avx2.inc)) produce the same sums as the generic ones
([`test_q5k.cpp`](qnx/llm/kernels/test_q5k.cpp)) and keep NVIDIA's weights byte-identical. On the host, generation runs 3% to
7% faster and the slowest token is 14% to 15% shorter; the prompt runs 9% slower
([`kernels.json`](eval/results/kernels.json)).

**What the model adds.** Its role is deliberately small: it turns flags into a plain sentence and picks among the actions
those flags allow. The larger result is the containment: a model of this size generating flat out, killed and hung, on the
same board as the oxygen, at no cost to any cot.

Full account: [docs/ADVISOR.md](docs/ADVISOR.md). Model, data and oversight: [docs/SYSTEM_CARD.md](docs/SYSTEM_CARD.md).

## What it will not do

- The advisor never sets oxygen. Its type and the model's have no path to any blender, and the kernel refuses the attempt.
- It gives no diagnosis, dose, treatment or setting. The nurse decides.
- Titra has been run against simulated babies, oximeters and blenders only, in QEMU. It makes no clinical claim.

## Results

### Fault matrix on QNX

| ID | Fault | Budget | Measured on the host clock | Result |
|---|---|---|---|---|
| F1 | Oximeter flags sensor off | alarm 2 s, fallback 32 s | alarm 0.7 s, fallback 29.7 s | pass |
| F2 | Oximeter cable cut | alarm 5 s, fallback 32 s | alarm 3.3 s, fallback 30.3 s | pass |
| F3 | Oximeter repeats its last frame | fallback 5 s | fallback 2.2 s | pass |
| F4 | SpO2, pulse and perfusion frozen | fallback 22 s | fallback 19.0 s | pass |
| F5 | Noise and corrupt frames on the line | no fallback | no fallback | pass |
| F6 | Oximeter driver killed | alarm 2 s, resume 10 s, no fallback | alarm 0.5 s, resume 1.5 s | pass |
| F7 | Controller killed | resume 10 s, no step | resume 0.7 s, step 0.00 | pass |
| F8 | Controller hung (stopped) | alarm 5 s, resume 15 s | alarm 2.7 s, resume 5.5 s, step 0.00 | pass |
| F9 | Blender driver killed | resume 10 s | resume 1.4 s | pass |
| F10 | Blender link cut | alarm 5 s, fallback 17 s | alarm 2.5 s, fallback 14.5 s | pass |
| F11 | Out-of-bounds commands to the blender driver | 100% refused | 100 of 100 refused | pass |
| F12 | Nurse station's type opens a blender driver | 100% refused | 100 of 100 refused | pass |
| F13 | Every core busy at lower priority, 300 s | 0 missed ticks | 0 missed ticks | pass |
| F14 | Controller of cot 2 killed: cots 1 and 3 watched | 0 missed ticks | step 0.00, 0 missed ticks | pass |
| F15 | Advisor's type opens a blender driver | 100% refused | 100 of 100 refused | pass |
| F16 | Model's process tries to leave its partition | 100% refused | 1400 of 1400 refused | pass |
| F17 | Model generates flat out for 10 minutes | 0 missed ticks | 0 missed ticks, 1189 tokens | pass |
| F18 | Model's process killed | resume 5 s, 0 missed ticks, page says unavailable | resume 1.4 s, 0 missed ticks, page said unavailable | pass |
| F19 | Model's process hung (stopped) | resume 20 s, 0 missed ticks | resume 11.5 s, 0 missed ticks, page said unavailable | pass |
| F20 | Injected note and malformed answers | 100% refused | 12 of 12 refused | pass |
| F21 | Urgent request during a long one | deadline met | deadline met, long request overtaken | pass |
| F22 | Priority 20 client messages the model | receiver stays at 6 | receiver at 6 | pass |
| F23 | Blender's converter refuses connections | alarm 5 s, fallback 17 s, 0 missed ticks | alarm 3.7 s, fallback 15.7 s, 0 missed ticks | pass |

23 of 23 cases pass. Target: QNX 8.0 in QEMU with hardware acceleration, x86_64, 4 CPUs.

During the fault run on QNX, the two cots left undisturbed kept 88.7% and 85.4% of the time in target over about
70 minutes.

Reproduce: boot the image ([On QNX 8.0](#on-qnx-80)), then run `python -m bench.live --types`. Results:
[`eval/results/faults.json`](eval/results/faults.json).

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

Reproduce: `python -m bench evaluate development`, `python -m bench evaluate held_out` and `python -m bench steps`.
Results: [`eval/results/development.json`](eval/results/development.json),
[`eval/results/held_out.json`](eval/results/held_out.json).

### The advisor

| What | Result, held-out | Reproduce | Results |
|---|---|---|---|
| Flags, 90 windows | Probe, hold or fallback and wean 100%, desaturations 97.5%, rising need 90%; 4 windows with a flag the case did not expect | `python -m bench flags held_out` | [`flags_held_out.json`](eval/results/flags_held_out.json) |
| Forecast, 11,848 minutes | AUROC 0.704 against 0.668 for the best rule; precision 60.2% against 49.5% at 2 alerts per cot per hour | `python -m bench risk held_out` | [`risk_held_out.json`](eval/results/risk_held_out.json) |
| Answers at 1, 2 and 4 loops, 54 asked | Valid 98.1%, 96.3%, 94.4%; flags the sentence speaks to 89.0%, 89.3%, 89.0%; first cot equals the rules' first 97.7% | `python -m bench model-score eval/results/model_outputs_held_out_L1.jsonl eval/model_windows_held_out.txt`; answers recorded with [`replay_host.cpp`](qnx/llm/replay_host.cpp) | [`model_outputs_held_out_L1.jsonl`](eval/results/model_outputs_held_out_L1.jsonl) |
| Timing on the board, 5 asked per configuration | 1, 2 and 4 loops: 11.95, 8.52 and 4.56 tok/s, mean answer 20.6, 30.4 and 55.9 s; under a 30 s deadline, 5 of 5 met, longest 27.4 s; 0 late ticks throughout | `python scripts/measure_board.py` | [`board.json`](eval/results/board.json) |

> [!IMPORTANT]
> What is real and what is simulated: the QNX processes, drivers, resource managers, restarts, security policy and the
> language model run on QNX 8.0 in QEMU. The baby, the oximeter and the blender are simulated on the host
> ([`docs/MODEL.md`](docs/MODEL.md)).

## Run it

### The simulation, no QNX needed

Linux or WSL, with `make`, a C11 compiler and Python 3.12.

```sh
make -C core test                      # build the C core and run its unit tests
python -m venv .venv && . .venv/bin/activate
pip install -r requirements-dev.txt
python -m pytest --cov                 # the bench, the C core through ctypes, and every recorded answer again
python -m bench steps                  # the step tests
python -m bench sim unstable 0         # one four-hour closed-loop run
python -m bench evaluate development   # every family, seed and controller
```

A clean clone holds everything these need. The QNX SDP, the model and the recordings of the live runs are outside it.

### On QNX 8.0

QNX SDP 8.0 under the free QNX Everywhere licence, on Ubuntu 22.04 or 24.04 (WSL works), with QEMU. The toolchain is qcc
(`gcc_ntox86_64`, GCC 12.2.0) with mkqnximage and secpolcompile. The model needs hardware acceleration to answer at usable
speed; the measured runs used the Windows Hypervisor Platform.

```sh
source ~/qnx800/qnxsdp-env.sh
qnx/llm/build.sh                                                    # llama.cpp for QNX 8.0, with the patch
make -C qnx all llm                                                 # qcc and q++, x86_64
qnx/llm/make_models_volume.sh dory.gguf models.qnx6                 # the model's disk; without it, flags alone
qnx/image/build.sh ~/titra-image --secpol=secure --policy=$PWD/qnx/image/titra.secpol
qnx/image/run.sh ~/titra-image models.qnx6 &                        # QEMU, KVM if present; ssh on :2222, station on :8080
python -m bench.live --types                                        # three simulated cots and the fault matrix
```

Then open http://127.0.0.1:8080 for the nurse station. Under the Windows Hypervisor Platform, build the image with
`TITRA_STARTUP_OPTS="-f 0,0,200M"` and boot it with [`qnx/image/run_whpx.ps1`](qnx/image/run_whpx.ps1), then run the bench
on Windows with `--ssh` and `--ssh-key`.

[`examples/`](examples/) holds one answer from start to finish, to read without running anything.

## Claims

| Claim | Code | Test |
|---|---|---|
| FiO2 never leaves the cot's limits or the blender driver's slew limit, whatever SpO2 does | [`control.c`](core/src/control.c), [`guard.c`](core/src/guard.c) | [`test_chain.py`](tests/test_chain.py) |
| SpO2 lost: FiO2 held, alarm at once, fallback 30 s after the last valid sample | [`control.c`](core/src/control.c) | [`test_control.c`](core/tests/test_control.c), [`test_chain.py`](tests/test_chain.py) |
| The fallback never sits below the backup FiO2 or the 5-minute baseline | [`control.c`](core/src/control.c) | [`test_control.c`](core/tests/test_control.c), [`test_chain.py`](tests/test_chain.py) |
| Repeated or frozen oximeter data is untrusted and its commands leave the baseline | [`quality.c`](core/src/quality.c) | [`test_quality.c`](core/tests/test_quality.c), [`test_control.c`](core/tests/test_control.c) |
| A frozen reading moves FiO2 by at most 20 rate steps before it is distrusted | [`control.c`](core/src/control.c) | [`test_control.c`](core/tests/test_control.c) |
| The blender driver refuses commands outside the limits, above the slew limit, or fallbacks below the backup | [`guard.c`](core/src/guard.c) | [`test_guard.c`](core/tests/test_guard.c) |
| The bench and the C core read and write identical frames | [`frames.py`](bench/frames.py), [`frame.c`](core/src/frame.c) | [`test_frames.py`](tests/test_frames.py) |
| Only each cot's controller may reach that cot's drivers, and the model and the advisor reach none of them, under the committed policy | [`titra.secpol`](qnx/image/titra.secpol) | [`test_policy.py`](tests/test_policy.py) |
| The flags rise on the cases that expect them and stay down on stable babies | [`facts.c`](core/src/facts.c) | [`test_facts.c`](core/tests/test_facts.c), [`test_flags.py`](tests/test_flags.py) |
| The forecast beats every rule baseline on held-out seeds, on the measure agreed before training | [`risk.c`](core/src/risk.c) | [`test_risk.c`](core/tests/test_risk.c), [`test_risk.py`](tests/test_risk.py) |
| The grammar gives a cot only words from its own flags and no digit | [`advice.c`](core/src/advice.c) | [`test_advice.c`](core/tests/test_advice.c) |
| The validator refuses a digit, a forbidden word, a word outside the cot's vocabulary, a healthy cot first, and malformed text | [`advice.c`](core/src/advice.c) | [`test_advice.c`](core/tests/test_advice.c), [`test_model_eval.py`](tests/test_model_eval.py) |
| Every recorded answer gets the same verdict from the C validator today, and no model sentence holds a digit | [`model_eval.py`](bench/model_eval.py) | [`test_model_eval.py`](tests/test_model_eval.py), [`test_claims.py`](tests/test_claims.py) |
| Earliest deadline first, and the largest loop count that meets the deadline | [`loops.c`](core/src/loops.c) | [`test_loops.c`](core/tests/test_loops.c) |
| The weights' digest is checked with SHA-256 against published vectors | [`sha256.c`](core/src/sha256.c) | [`test_sha256.c`](core/tests/test_sha256.c) |
| The figures on this page match the committed result files | [`eval/results/`](eval/results/) | [`test_claims.py`](tests/test_claims.py) |
| The spec's figures match their published abstracts and the case set | [`reference.json`](eval/reference.json) | [`test_eval_cases.py`](tests/test_eval_cases.py) |

Tests: `make -C core test` runs 173 C tests; `python -m pytest --cov` runs 297 Python tests (274 unit, 23
integration) in a new random order each time, each within 30 seconds, with no network and no keys. Coverage of the C
core by gcov: 94 to 100% of lines and 82 to 100% of branches per file (`make -C core coverage`). Coverage of the bench
by coverage.py: 96% of lines and branches; the code that drives the QNX guest over ssh runs only against the guest and
is excluded, as set in [`pyproject.toml`](pyproject.toml). CI runs both on every push.

## Layout

- `core/` the deterministic C11 core and its unit tests: control law, signal quality, frames, guard, facts, forecast,
  grammar and validator, loop choice
- `qnx/` the QNX processes, the launcher, the nurse station, the advisor, `/dev/llm` with the llama.cpp patch and kernels,
  and the image recipe with its security policy
- `bench/` the bench twin: simulated baby, oximeter and blender, reference controllers, evaluation, tuning, and the live
  bench that runs the fault matrix against the QNX guest
- `eval/` the case sets, the tuning log, the recorded answers and the results
- `examples/` one answer from start to finish
- `docs/` the specification, the advisor, the system card, the model, the line protocol and the figures
- `tests/` the bench's tests, including the C core through ctypes
- `scripts/` figure, capture, training and measurement tools

## License

[MIT](LICENSE). The model is not in the repository: NVIDIA Nemotron Labs Dory 1.0 4B, licence OpenMDW-1.1. llama.cpp is
MIT.

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
