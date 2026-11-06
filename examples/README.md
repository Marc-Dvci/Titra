# One answer, from start to finish

A held-out case window (`w0-s1000-e10800`): a ward of three simulated babies, the last three hours. Everything here is
simulated. Written by [`scripts/make_example.py`](../scripts/make_example.py) from the C core and the recorded answer,
and checked against them by [`tests/test_examples.py`](../tests/test_examples.py).

| Step | File | Written by |
|---|---|---|
| The facts and flags of each cot | [`facts.json`](facts.json) | C, from the controller's status |
| The prompt | [`prompt.txt`](prompt.txt) | C, from the facts |
| The grammar for this request: each cot's words, actions and order | [`grammar.gbnf`](grammar.gbnf) | C, from the flags |
| The answer, 92 tokens | [`answer.json`](answer.json) | The model, inside that grammar |
| The verdict: `ok` | | C, the validator, trusting nothing from the grammar |

## What the nurse reads

**1. Cot 3.** Review respiratory support. frequent desaturations and probe trouble mean the nurse should review respiratory support and check the probe site. Chance of a dip in 10 minutes: 59%.

Flags, from C: frequent_desaturations, probe_trouble, hold_or_fallback. Record, from C: Last 180 minutes: time in target 88 percent; 9 desaturation episodes; longest 20 seconds; FiO2 from 0.28 to 0.29; 41 probe-off events; 11 percent no valid reading; 885 seconds in hold; 329 seconds in fallback; 4 percent above 95 on oxygen; 2 percent below 85. Flags: frequent desaturations, probe trouble, hold or fallback.

**2. Cot 2.** Review respiratory support. rising need and frequent desaturations mean the nurse should review respiratory support. Chance of a dip in 10 minutes: 34%.

Flags, from C: rising_need, frequent_desaturations. Record, from C: Last 180 minutes: time in target 91 percent; 6 desaturation episodes; longest 16 seconds; FiO2 from 0.24 to 0.27; 5 probe-off events; 1 percent no valid reading; 101 seconds in hold; 3 percent above 95 on oxygen; 1 percent below 85. Flags: rising need, frequent desaturations.

**3. Cot 1.** Nothing flagged. Time in target 95 percent. Chance of a dip in 10 minutes: 8%.

Flags, from C: none. Record, from C: Last 180 minutes: time in target 95 percent; 1 desaturation episode; longest 12 seconds; FiO2 from 0.22 to 0.22; 8 probe-off events; 2 percent no valid reading; 153 seconds in hold; 25 seconds in fallback; 3 percent above 95 on oxygen. Flags: none.

The sentences of flagged cots are the model's; cot 1 has no flag, so its sentence is the rules' own and the model
wrote nothing for it. Every number on the page comes from C.
