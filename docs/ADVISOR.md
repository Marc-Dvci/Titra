# The on-board advisor

The advisor tells a night-shift nurse which cot to go to first, and writes the shift handover. It runs on the same QNX
8.0 board as the oxygen controllers, with no network. NVIDIA Nemotron Labs Dory 1.0 4B, a looped language model, runs
beside the controllers as the resource manager `/dev/llm`. The kernel keeps it away from every blender.

**The boundary, in one sentence:** C code computes every number and every flag and decides the order of the tiers; the
model only writes the sentence from words those flags allow; the kernel refuses any path from the model to a valve.

[What the nurse sees](#what-the-nurse-sees) · [How an answer is made](#how-an-answer-is-made) ·
[What the kernel enforces](#what-the-kernel-enforces) · [A looped model under deadlines](#a-looped-model-under-deadlines) ·
[The port](#the-port-to-qnx-80) · [Measured](#measured) · [Reproduce](#reproduce) · [Limits](#limits)

## What the nurse sees

- **Unit watch.** Every 15 minutes, and within a minute when a cot's flags change, each cot gets an action and a sentence,
  in the order to visit them: "frequent desaturations and probe trouble mean the nurse should review respiratory support
  and check the probe site." Beside the sentence, from C: the chance of a dip within ten minutes and the cot's record.
- **Shift handover.** On demand, a record per cot from the controller's own history, with a sentence for each flagged cot.
- **Incident notes for the biomedical engineer.** What failed, what Titra did, for how long, and whether FiO2 moved. C
  writes these alone.
- **A nurse's note.** It goes to the model as quoted data, cut to 120 characters, and is shown as the nurse wrote it.

The page always says where a note came from: the model checked against the facts, or the flags alone. It says when the
model is unavailable, and the flags carry on.

## How an answer is made

| Step | What runs | What it decides |
|---|---|---|
| 1. Facts | [`core/src/facts.c`](../core/src/facts.c), on the controller's status, once a second | Time in target, desaturation episodes, the FiO2 trend, probe events, holds and fallbacks, restarts |
| 2. Flags | The same file | Seven flags, with thresholds tuned on development seeds only |
| 3. Forecast | [`core/src/risk.c`](../core/src/risk.c): 60 trees of depth 2, plain C, no allocation | One number: the chance of a dip below 85% within ten minutes |
| 4. Prompt and grammar | [`core/src/advice.c`](../core/src/advice.c) | A GBNF grammar composed for this request: per-cot vocabulary from that cot's flags, no digit, actions the flags allow, every cot once, higher tiers first |
| 5. The model | [`qnx/llm/llm.cpp`](../qnx/llm/llm.cpp), `/dev/llm` | The wording, the action within the allowed list, and the order within a tier |
| 6. Validator | [`core/src/advice.c`](../core/src/advice.c) again, trusting nothing from step 4 | Whether the answer is shown at all |
| 7. The page | [`qnx/station/index.html`](../qnx/station/index.html) | Every number and flag comes from C and sits beside the sentence, so a sentence cannot change a fact or hide one |

A cot with no flag gets the rules' own sentence with no model text. A ward with no flag does not ask the model. If the
model is unavailable, late or refused, the page shows the flags in the rules' order.

## What the kernel enforces

| Guarantee | QNX mechanism | Code | Measured by |
|---|---|---|---|
| No path from the model or the advisor to a blender | Security types `llm_t` and `advisor_t`; `channel_connect` lists only what each needs | [`qnx/image/titra.secpol`](../qnx/image/titra.secpol), [`check_policy.py`](../qnx/image/check_policy.py) | F15, F16 |
| The model cannot leave its partition: no socket, no spawn, no physical memory, no I/O privilege, no priority 30, no write to its weights, no 16 GB | The same type, user 300 after attaching, `RLIMIT_AS`, sealed read-only weights | [`llm.cpp`, `escape_attempts()`](../qnx/llm/llm.cpp) | F16 |
| Inference never competes with control | Controllers at priority 30 on CPU 0; inference at priority 5 on CPUs 1-3, a cluster declared at boot (`startup-x86 -c advice:0xe`); `procnto -P 22` caps everything without the priority ability | [`qnx/image/build.sh`](../qnx/image/build.sh), [`llm.cpp`](../qnx/llm/llm.cpp) | F13, F17 |
| No client can lift inference | The channel is `_NTO_CHF_FIXED_PRIORITY`: a client at priority 20 leaves the receiving thread at 6 | [`llm.cpp`](../qnx/llm/llm.cpp) | F22 |
| A dead model costs seconds, and a hung model is noticed | HAM restart from its own command line; the heartbeat is inference progress, not liveness | [`llm.cpp`, watchdog](../qnx/llm/llm.cpp) | F18, F19 |
| A restart does not reload 3.67 GB | `llmstore` fills one sealed shared memory object at boot, checks its SHA-256 and exits; `llm` maps it read-only | [`qnx/llm/llmstore.c`](../qnx/llm/llmstore.c) | F18 |
| A slow blender link cannot hold the controller | `valved` writes the link from its own thread, never under the mutex the controller's message needs | [`qnx/valved/valved.c`](../qnx/valved/valved.c) | F23 |

## A looped model under deadlines

Dory has 8 input layers, 20 recurrent layers and 8 output layers, and the recurrent block runs one to four times per
token. That makes it an imprecise computation in the sense of Liu and Layland: the input block, one loop and the output
block are mandatory, and each further loop is optional work the scheduler may drop.

Two pure functions in [`core/src/loops.c`](../core/src/loops.c), tested on the host, turn this into real-time scheduling on the
board:

- **`tl_choose_loops`** picks, at every token, the largest loop count that still meets the request's deadline, from cost
  per token measured live on this board.
- **`tl_pick_edf`** picks, at every token boundary, the session with the earliest deadline. One `llama_context` holds a
  sequence per session, so an urgent request overtakes a long one at the next token, at no switching cost (F21).

The upstream `llama.cpp` fixes the loop count per context. The patch adds `llama_set_n_loops()`, so the count can change
per decode.

## The port to QNX 8.0

I found no public port of llama.cpp to QNX 8.0. This one builds from the official repository at the commit NVIDIA's model
card pins, with [`qnx/llm/patches/llama-qnx.patch`](../qnx/llm/patches/llama-qnx.patch):

- the QNX CMake toolchain ([`toolchain-qnx-x86_64.cmake`](../qnx/llm/toolchain-qnx-x86_64.cmake)) and the memory query;
- thread affinity through QNX runmasks and fixed thread priority in ggml's pool;
- `llama_set_n_loops()`, for the per-decode loop count;
- new AVX2 kernels for Q5_K weights in the 8x8 repacked layout ([`q5k_avx2.inc`](../qnx/llm/kernels/q5k_avx2.inc)), which
  had no x86 path: the recurrent block's matrix products set the speed of every token. They produce the same sums as ggml's
  generic kernels on 48,672 products, checked by [`test_q5k.cpp`](../qnx/llm/kernels/test_q5k.cpp). NVIDIA's weights stay
  byte-identical.

The kernels trade a little prompt speed for a steadier generation. The same binary with repacking off and on, 3 threads, on
the Linux host ([`kernels.json`](../eval/results/kernels.json)):

| Loops | Generation, before and after | Slowest token, before and after | Prompt, before and after |
|---|---|---|---|
| 1 | 15.99 to 16.47 tok/s | 74 to 63 ms | 59.7 to 54.6 tok/s |
| 4 | 6.72 to 7.16 tok/s | 168 to 145 ms | 22.7 to 20.6 tok/s |

Booting under the Windows Hypervisor Platform takes three more settings, all in
[`qnx/image/run_whpx.ps1`](../qnx/image/run_whpx.ps1) and [`build.sh`](../qnx/image/build.sh):
`-cpu host,+arat,x-force-features=on` (QNX startup requires CPUID.6 ARAT, which Hyper-V hides), `startup-x86 -f 0,0,200M`
(the 200 MHz APIC timer, which the startup library cannot detect there), and a runmask that matches a declared cluster.

## Measured

Everything below describes simulated babies. The case sets were committed before the code that scores them:
[flags](../eval/flag_cases.json), [forecast](../eval/risk_cases.json), [model](../eval/model_cases.json). Thresholds, the
forecast and the prompts were set on development seeds. The held-out seeds ran once, after they were frozen.

### Flags, on 90 held-out windows

| Flag | Recall | Windows expected |
|---|---|---|
| Probe trouble | 100% | 20 |
| Hold or fallback | 100% | 20 |
| Wean opportunity | 100% | 10 |
| Frequent desaturations | 97.5% | 40 |
| Rising need | 90% | 10 |

4 of the 90 windows carry a flag the case did not expect, and 2 of the 20 stable windows raise a flag.
Results: [`flags_held_out.json`](../eval/results/flags_held_out.json).

### Early warning, on held-out seeds

At each sampled minute, from what the advisor can see in the past hour: will a dip below 85% start within ten minutes?
24.3% of the 11,848 scored minutes have one ahead. The model was to be kept only if it beat the best rule on both measures.

| Scorer | AUROC | Precision at 2 alerts per cot per hour | Share of dips caught |
|---|---|---|---|
| Forecast (60 trees, depth 2) | 0.704 | 60.2% | 8.2% |
| SpO2 variability, last 15 min | 0.668 | 45.9% | 6.3% |
| Dips below 85%, last hour | 0.657 | 49.5% | 6.8% |
| Time below 91%, last 15 min | 0.654 | 25.6% | 3.5% |

It ranks cots by risk and does not predict individual events. Results:
[`risk_held_out.json`](../eval/results/risk_held_out.json).

### The language model's answers, on 70 held-out windows

54 windows have a flag on the ward; the other 16 are healthy wards where the model is not asked. Each answer goes through
the C validator again, in CI, with no model and no QNX.

| Loops | Valid answers | Flags the sentence speaks to | First cot equals the rules' first |
|---|---|---|---|
| 1 | 53 of 54 (98.1%) | 153 of 172 (89.0%) | 97.7% |
| 2 | 52 of 54 (96.3%) | 150 of 168 (89.3%) | 97.7% |
| 4 | 51 of 54 (94.4%) | 146 of 164 (89.0%) | 97.7% |

No sentence from the model carries a digit, a dose, a setting or a word outside the cot's vocabulary. On these cases
more loops did not raise any score, so the board runs one loop when a deadline is tight and the elastic controller decides
the rest. The answers are in [`eval/results/model_outputs_held_out_L1.jsonl`](../eval/results/model_outputs_held_out_L1.jsonl)
and its siblings.

**What the model adds.** Its role is deliberately small. The sentence is its contribution: it turns flags into a plain
instruction, and picks the action among those the flags allow. At one loop the 155 sentences on the page are 31 distinct
ones, because the vocabulary is closed and the cases repeat. The order of the tiers is the rules', and the facts are C's.
The larger result is the one the fault matrix measures: a 3.67 GB model generating flat out on the same board as the
controllers, at no cost to them.

### On the board: timing

Measured on QNX 8.0 under WHPX, three threads on CPUs 1-3, with the three cots under control, on 5 asked answers per
configuration taken evenly across the held-out windows ([`eval/results/board.json`](../eval/results/board.json)):

| Configuration | Generation | Prompt | Mean answer | Longest answer | Deadlines met | Controllers' late ticks | Longest tick interval |
|---|---|---|---|---|---|---|---|
| 1 loop | 11.95 tok/s | 27.3 tok/s | 20.6 s | 23.4 s | 5 of 5 | 0 | 1.002 s |
| 2 loops | 8.52 tok/s | 18.1 tok/s | 30.4 s | 35.4 s | 5 of 5 | 0 | 1.002 s |
| 4 loops | 4.56 tok/s | 9.9 tok/s | 55.9 s | 59.6 s | 5 of 5 | 0 | 1.027 s |
| Elastic, 30 s deadline | 10.14 tok/s | 24.8 tok/s | 23.2 s | 27.4 s | 5 of 5 | 0 | 1.027 s |
| Elastic, 120 s deadline | 4.41 tok/s | 9.4 tok/s | 58.8 s | 65.3 s | 5 of 5 | 0 | 1.027 s |

Under the 30 s deadline the controller ran 368 tokens at one loop and 12 at two. Under 120 s, 348 at four, 1 at three and
32 at two. The product's watch runs under the 30 s deadline.

Facts for a three-hour window take 62.9 µs on average and 841.6 µs at most; a forecast takes 0.21 µs on average and
11.06 µs at most, over 1,000 rounds on the board at the advisor's priority.

At boot, `llmstore` loads the weights into shared memory and checks their SHA-256 in about 30 s, and `llm` is ready about
27 s later. After a restart it is ready in under a second (F18, F19).

### Containment

The fault matrix on QNX, with the security policy enforced and every time taken on the host clock:
[README](../README.md#fault-matrix-on-qnx), [`eval/results/faults.json`](../eval/results/faults.json). F15 to F23 are
the cases for the model and its partition.

## Reproduce

```sh
python -m bench flags held_out                         # the flags, from the simulator
python -m bench risk held_out                          # the forecast and its baselines
python -m bench model-cases held_out                   # the windows the model replays
python -m bench model-score eval/results/model_outputs_held_out_L1.jsonl eval/model_windows_held_out.txt
qnx/llm/kernels/run_test.sh                            # the kernels against ggml's generic ones
python scripts/measure_board.py                        # timing on the board, with the guest up and the bench connected
python scripts/train_risk.py                           # the forecast's training, development seeds only
```

Answers are recorded with [`qnx/llm/replay_host.cpp`](../qnx/llm/replay_host.cpp). The kernels' before and after comes from
[`qnx/llm/llm_bench.cpp`](../qnx/llm/llm_bench.cpp), where `-X` turns repacking off.

The model is not in the repository: NVIDIA Nemotron Labs Dory 1.0 4B, Q5_K_M GGUF, 3,673,288,864 bytes, SHA-256
`96080be49ffd67b916018b7bdef8c484762576fb52780e10ab98ac4978a660e0`, licence OpenMDW-1.1.
[`make_models_volume.sh`](../qnx/llm/make_models_volume.sh) puts it on the board's second disk. `titra-launch` starts
`llm` only if its digest matches. Without the model the advisor runs on its flags alone.

## Limits

- The flags and the forecast have seen only the bench's simulated babies.
- A sentence from allowed words can still read awkwardly, and it can leave a flag out. The flags and the facts are always
  on the same row.
- The model writes English only.
- Under the Windows hypervisor, a boot occasionally hangs with the model's CPUs pinned and ssh silent (about one boot in
  twelve here); booting again clears it.
