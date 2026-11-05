# The bench twin: simulated baby, oximeter and blender

The bench twin runs on the host, outside the QNX guest. For each cot it simulates a preterm baby, the oximeter on the
baby's foot and the air-oxygen blender on the CPAP circuit. It talks to the guest over one serial line to the oximeter
driver and one to the blender driver, exactly as the devices would. Every parameter below is a design assumption unless
a source is given, and each scenario draws its parameters from the ranges in `eval/scenarios.json`.

The model advances in steps of 1 s. First-order lags use the exact discrete form `x += (target - x) * (1 - exp(-dt/tau))`.

## Oxygen delivery

| Stage | Model | Value |
|---|---|---|
| Blender servo | Moves toward the commanded FiO2 at up to 0.05 per second | |
| Blender mixing | First-order lag | tau = 1 s |
| CPAP circuit wash-in | First-order lag; tau is circuit volume over gas flow, about 0.75 L at 7 L/min | tau = 6 s |
| Lung wash-in | First-order lag; tau is functional residual capacity over alveolar ventilation, about 25 mL/kg over 100 mL/kg/min | tau = 15 s |

The circuit FiO2 is reported by the blender twin as the measured FiO2, the way an oxygen analyser in the circuit would
report it.

## Gas exchange

Alveolar oxygen tension follows the alveolar gas equation:

```
PAO2 = FA * (PB - 47) - PaCO2 / 0.8
```

with `FA` the alveolar oxygen fraction, `PB` = 745 mmHg (barometric pressure near 230 m altitude) and `PaCO2` drawn
per scenario.

Lung disease enters through two parameters, as in clinical descriptions of preterm oxygenation:

- **Ventilation-perfusion mismatch** lowers end-capillary oxygen tension by a pressure deficit `dP` (mmHg):
  `PcO2 = PAO2 - dP`.
- **Shunt** `f` is the fraction of cardiac output that bypasses ventilated alveoli and carries mixed venous blood.

Oxygen content is bound plus dissolved oxygen, `C(P) = 1.34 * Hb * S(P) + 0.003 * P` (mL/dL), with `Hb` = 15 g/dL.
Mixed venous content is the arterial content minus the arteriovenous difference `D`. Solving the mixing equation
`Ca = (1 - f) * Cc + f * (Ca - D)` gives:

```
Ca = Cc - f / (1 - f) * D
```

`D` = 3 mL/dL, from an oxygen consumption of 7 mL/kg/min over a cardiac output near 230 mL/kg/min. The arterial
saturation is the saturation of blood with content `Ca`, found by bisection on the tension.

Saturation comes from the Severinghaus dissociation curve, shifted to a neonatal P50 of 20 mmHg by scaling the
tension:

```
S(P) = 1 / (23400 / (Q^3 + 150 Q) + 1),   Q = P * 26.8 / 20
```

Over the scenario ranges this gives steady-state FiO2 needs from 0.21 to about 0.45 for 93% saturation, and 1 to 2
saturation points per 1% FiO2 around the target.

Arterial blood reaches the oximeter site after a 5 s circulation delay.

## Disturbances

- **Apnoea.** Episodes arrive as a Poisson process. During an apnoea the alveolar oxygen fraction decays
  exponentially at 0.7% per second. Oxygen consumption alone would empty a 25 mL/kg lung store about twice as fast,
  but the blood's oxygen store buffers the fall, and the rate is chosen so that apnoeas of 8 to 25 s produce nadirs
  mostly between 75% and 90%. When breathing resumes the alveolar fraction washes in again toward the circuit FiO2.
  Pulse rate falls by up to 60 /min during the longer apnoeas.
- **Shunt episodes.** Episodes of atelectasis or secretions raise the shunt fraction by 0.05 to 0.15 for 5 to 20
  minutes.
- **Drift.** The shunt fraction follows a slow mean-reverting random walk (time constant 30 minutes). Worsening and
  improving scenarios move its mean, and `dP`, linearly over the run.

## Oximeter

- An 8 s moving average of the saturation at the site, plus Gaussian noise (standard deviation 1%), rounded to a whole
  percent.
- Pulse rate near 155 /min with beat-to-beat variation, and a perfusion index near 1.5% with slow variation.
- **Flagged artefact.** Motion episodes lower the reading by 5 to 20 points with the interference flag set.
- **Unflagged artefact.** Motion episodes lower the reading by 5 to 15 points with no flag. These are the readings an
  oximeter gets wrong without knowing it.
- **Sensor off.** Episodes of 10 to 120 s with the sensor-off flag set and no value.
- One frame per second in the line format of `docs/PROTOCOL.md`.

## Blender

The blender applies each command frame through the servo model above. It keeps its own watchdog, as set out in the
safety layers of `docs/SPEC.md`:

- after 3 s with no valid frame it holds and sounds its alarm;
- after 15 s it applies the fallback FiO2, computed from the frames it has received.

## Reference controllers

These run on the same scenarios as Titra, for comparison:

| Reference | Behaviour |
|---|---|
| Fixed FiO2 | Set once at the start to the FiO2 that holds 93% at the scenario's starting physiology, then left alone |
| Manual-care model | A nurse who responds to an alarm (SpO2 below 88% for 10 s, or above 95% on oxygen for 60 s) after a delay drawn from an exponential distribution with a 3-minute mean, changes FiO2 by 0.05 (0.10 below 80%), looks again a minute later, and also checks every 60 minutes |

The manual-care model is a set of rules chosen for this bench. It is not fitted to the trial data.
