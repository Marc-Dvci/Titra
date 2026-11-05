# Titra

**Open oxygen control for preterm babies on low-cost CPAP**

[![Tests](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Titra reads a baby's SpO2 once a second, sets the air-oxygen blender to hold the 91-95% target range, and puts the cot
in a defined safe state when a sensor, a cable or a piece of its own software fails. One QNX 8.0 board serves several
cots, and a failure in one cot leaves the others under control.

In a randomised crossover trial in southwest Nigeria, preterm babies on low-cost CPAP spent 30.1% of the time in the
target range under manual control and 88.1% under automated control[^1]. The algorithm in that trial is licensed to one
manufacturer. Titra is an independent controller under the MIT licence.

## Specification

- [`docs/SPEC.md`](docs/SPEC.md): users, safety layers, signal quality rules, the fault matrix and the evaluation.
- [`docs/MODEL.md`](docs/MODEL.md): the simulated baby, oximeter and blender of the bench.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md): the serial line formats to the oximeter and the blender.

## Results on the development scenarios

60 four-hour runs per controller, six scenario families of simulated preterm babies, the same babies and oximeters
for every controller. Time in target uses the trial's definition on displayed SpO2.

| Controller | Time in target | Below 80% | Above 98% on oxygen | FiO2 adjustments per minute |
|---|---|---|---|---|
| Titra | 89.3% | 0.41% | 0.11% | 1.24 |
| Fixed FiO2, set to the baby's starting need | 64.6% | 9.51% | 0.94% | 0 |
| Manual-care model | 74.5% | 0.69% | 0.44% | 0.02 |

Titra's time in target is higher than fixed FiO2 on 46 of the 60 runs. In the bench's step tests (SpO2 held at 80% and
at 98%), the largest FiO2 change in any minute is 0.090 up and 0.100 down. The six commercial devices measured by
Salverda et al. on the same protocol range from 0.062 to 0.30 up[^2].

These figures describe the simulated babies of [`docs/MODEL.md`](docs/MODEL.md). The controller was tuned on these
development seeds only ([`bench/tune.py`](bench/tune.py), [`eval/tuning_log.jsonl`](eval/tuning_log.jsonl)). The
held-out seeds are fixed in [`eval/scenarios.json`](eval/scenarios.json) and are run once, after tuning ends. Full
results: [`eval/results/development.json`](eval/results/development.json).

## Layout

- `core/` the deterministic C11 core shared by the QNX processes and the bench: frame codecs, signal quality,
  controller, baseline, blender-driver guard and link watchdog, with its unit tests
- `bench/` the bench twin in Python: simulated baby, oximeter and blender, reference controllers, metrics, closed-loop
  runs, step tests and the tuner
- `eval/` the case set (fault matrix, scenarios, step tests, published references), the tuning log and the results
- `tests/` the bench's tests, including the C core driven through ctypes
- `docs/` specification, model and protocol

## Run it

Linux or WSL, with `make`, a C11 compiler and Python 3.12.

```sh
make -C core test                      # build the core and run its 79 unit tests
python -m venv .venv && . .venv/bin/activate
pip install -r requirements-dev.txt
python -m pytest --cov                 # 138 tests of the bench and the core through ctypes
python -m bench steps                  # the step tests
python -m bench sim unstable 0         # one four-hour closed-loop run
python -m bench evaluate development   # every family, seed and controller
```

## License

[MIT](LICENSE).

[^1]: Subhi R et al. Lancet Glob Health 2025. https://doi.org/10.1016/S2214-109X(24)00458-3
[^2]: Salverda HH et al. Arch Dis Child Fetal Neonatal Ed 2026. https://doi.org/10.1136/archdischild-2026-330785
