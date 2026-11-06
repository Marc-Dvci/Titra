# System card: the on-board advisor

## What it is for

The advisor helps one night-shift nurse decide which of several cots to go to first, and writes the shift handover. It
reads what each cot's controller reports. It runs on the same QNX board as the controllers and needs no network.

## What it is not for

- It never sets oxygen. No part of it has a path to a blender: the kernel refuses the attempt ([faults F15 and F16](SPEC.md#fault-matrix)).
- It gives no diagnosis, dose, treatment or oxygen setting. The words for them are not in what it can write.
- It does not replace a nurse's assessment, or any alarm. The controllers raise their alarms whether or not it runs.
- It has been run against simulated babies only. It makes no clinical claim.

## The parts

| Part | What it is | What it decides |
|---|---|---|
| Facts and flags | C code over the controller's own status ([`core/src/facts.c`](../core/src/facts.c)) | Every number, every flag, the rules' order of the cots |
| Forecast | 60 small decision trees, trained on simulated babies ([`core/src/risk.c`](../core/src/risk.c)) | One number: the chance of a dip below 85% within ten minutes |
| Language model | NVIDIA Nemotron Labs Dory 1.0 4B, Q5_K_M, as published | The sentence about a flagged cot, its action from the allowed list, and the order of cots within a tier |
| Validator | C code ([`core/src/advice.c`](../core/src/advice.c)) | Whether an answer is shown at all |

The notes for the biomedical engineer are written by C alone: what failed, what Titra did, for how long, and whether FiO2
moved.

## How an answer is constrained

1. The prompt is composed from the facts. A nurse's note goes in as quoted data, cut to 120 characters.
2. A grammar composed for that request limits what the model can write: words only from a vocabulary built from that cot's
   flags, no digit, actions only from the list those flags allow, every cot exactly once, higher tiers first. A cot without
   a flag has no word to claim that flag with. There is no word for reassurance or a setting, and no note can add one.
3. A cot with no flag gets the rules' own sentence, with no model text. If no cot has a flag, the model is not asked.
4. The validator checks the answer again without trusting the grammar: the exact shape, no digit, forbidden wording, every
   word against the cot's vocabulary, a sentence that ends properly, the order.
5. Every number and every flag on the page comes from C and sits beside the sentence, so a sentence cannot change a fact or
   hide one by leaving it out.
6. If the model is unavailable, late, or refused, the page shows the deterministic flags, in the rules' order, and says so.

## Data

The forecast is trained on the development seeds of the bench's simulated preterm babies, and the thresholds of the flags
are tuned on the same seeds. No patient data is used anywhere. The held-out seeds are scored once, after every choice is
final. These figures describe simulated babies.

## Evaluation

Results are in [ADVISOR.md](ADVISOR.md#measured). The case sets are committed before the code that scores them:
[flags](../eval/flag_cases.json), [forecast](../eval/risk_cases.json), [model](../eval/model_cases.json). CI runs every
recorded answer through the validator again.

## Human oversight

The nurse decides. The page says where each note came from: the model checked against the facts, or the flags alone. The
system has no way to act on its own advice.

## Known limits

- The flags and the forecast have only seen the bench's simulated babies. The forecast ranks cots by risk and does not
  predict individual events.
- A sentence from allowed words can still read awkwardly. The validator refuses words strung together and sentences that go
  round in a circle, but it does not understand grammar.
- A sentence can leave a flag out. The flags and the facts are always on the same row.
- The model writes English only.
