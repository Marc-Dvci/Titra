# Serial line formats

Titra talks to each oximeter and each blender over its own serial line, at 115200 baud, 8 data bits, no parity, one
stop bit. Every frame is one ASCII line:

```
$<type>,<field>,<field>,...*<checksum>\r\n
```

- `<checksum>` is the XOR of every byte between `$` and `*`, written as two uppercase hexadecimal digits.
- A line is at most 80 bytes, including `\r\n`. A longer line is discarded up to the next `$`.
- A field with no value is written `--`.
- A frame with a bad checksum, a missing or extra field, or a value outside its range is rejected whole.

## Oximeter to driver: `$OX`

Sent once a second.

```
$OX,<clock>,<spo2>,<pr>,<pi>,<flags>*<checksum>
$OX,7261,93,156,1.48,00*12
```

| Field | Type | Range | Meaning |
|---|---|---|---|
| `clock` | unsigned integer | 0 to 4294967295 | Seconds since the oximeter started |
| `spo2` | integer or `--` | 0 to 100 | Saturation, percent |
| `pr` | integer or `--` | 0 to 400 | Pulse rate, per minute |
| `pi` | decimal with two places or `--` | 0.00 to 20.00 | Perfusion index, percent |
| `flags` | two hexadecimal digits | 00 to 1F | Status bits below |

| Bit | Name | Meaning |
|---|---|---|
| `0x01` | `SENSOR_OFF` | The sensor is off the patient |
| `0x02` | `NO_SENSOR` | No sensor is connected |
| `0x04` | `PULSE_SEARCH` | The oximeter is searching for a pulse |
| `0x08` | `LOW_PERFUSION` | The perfusion is too low for a reliable reading |
| `0x10` | `INTERFERENCE` | Motion or light interference |

Any set bit makes the sample invalid.

## Driver to blender: `$BL`

Sent by the blender driver once a second, and each time the controller sends a new command. Each frame is both a
command and a heartbeat.

```
$BL,<seq>,<fio2>,<backup>,<alarm>*<checksum>
$BL,412,305,300,0*0C
```

| Field | Type | Range | Meaning |
|---|---|---|---|
| `seq` | unsigned integer | 0 to 65535, wrapping | Frame counter |
| `fio2` | integer | 210 to 1000 | Commanded FiO2, per mille |
| `backup` | integer | 210 to 1000 | The cot's backup FiO2, per mille |
| `alarm` | integer | 0 to 3 | 0 none, 1 low, 2 medium, 3 high priority |

## Blender to driver: `$BS`

Sent once a second, and in reply to each `$BL` frame.

```
$BS,<seq>,<fio2>,<state>*<checksum>
$BS,412,301,N*76
```

| Field | Type | Range | Meaning |
|---|---|---|---|
| `seq` | unsigned integer | 0 to 65535 | The `seq` of the last `$BL` frame applied |
| `fio2` | integer | 0 to 1000 | FiO2 measured in the circuit, per mille |
| `state` | letter | `N`, `H`, `F` | Normal, holding after a lost link, fallback applied |
