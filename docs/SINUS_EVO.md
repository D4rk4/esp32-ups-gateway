# Inform Sinus EVO: gateway capabilities and RS-232 protocol

This document describes what the ESP32 gateway can read and do with Inform
Sinus EVO 2/3 kVA UPS units over RS-232, and what it is based on. The protocol
is classic MegaTec ("Q1"), 2400 baud, 8N1, every command and every answer ends
with `CR` (`\r`).

Sources:
- real exchange with the owner's Sinus EVO 3000 (Q1, F, I; 365 polls without
  errors; `BT` and `BP` answered);
- static analysis of the SNMP card firmware KSTAR KPM220 v1.4.27 (`kstar-snmp`)
  and its web interface — details and addresses in [KSTAR_PROTOCOL.md](KSTAR_PROTOCOL.md);
- publicly available MegaTec protocol description.

The card firmware is not the UPS firmware. It shows which commands the KSTAR
card sends to the UPS in the "MegaTec-1:1" profile, but does not prove that a
specific UPS executes them. The UPS response to management commands in the
protocol is not provided: the gateway reports "sent, execution not confirmed"
and checks the result on the next Q1 poll.

## Monitoring

| Request | What it provides | In the gateway |
| --- | --- | --- |
| `Q1` | input voltage, voltage of the last grid failure, output voltage, load %, input frequency, "raw" battery value, temperature, 8 status bits | poll every `poll_ms` |
| `F` | nominal output voltage, current, battery voltage, frequency | up to two attempts, repeat every 10 minutes |
| `I` | manufacturer, model, UPS firmware version; in the 61-byte variant, 21 more bytes which the gateway shows as the serial number | up to two attempts, repeat every 10 minutes |

Accepted `I` response variants accepted by the gateway (length with `CR`;
prefix `#` or `!`):

| Length | Fields |
| --- | --- |
| 34 | manufacturer 10 chars, model 10, version 10 |
| 39 | manufacturer 15, model 10, version 10 (that is how the owner's Sinus EVO
responds: empty manufacturer and model, version `V142B000D0`) |
| 61 | as 39 + 21 serial number characters |

The serial number in the 61-byte variant is derived: the KSTAR card writes
these 21 bytes into the same cell as the response to its own command "KSTAR
serial number" (`NQU`). The gateway shows it in the web interface and over SNMP
(`upsAdvIdentSerialNumber`) if present.

Q1 status bits (left to right): grid failure; battery low; bypass/AVR; UPS
fault; UPS type (1 = standby); test in progress; shutdown in progress; beeper
on.

### Battery

Sinus EVO transmits in Q1 the voltage **of one cell** (on the owner's, 2.24–2.25
V). The gateway computes the battery pack voltage as `raw × battery_multiplier`;
the multiplier is set on the Serial / UPS page. For 6 packs of 12 V (36 cells
of 2 V) the multiplier = 36, i.e. 2.25 × 36 ≈ 81 V. A value of 0 disables the
calculation; the raw value is always shown.

The KSTAR card does the same: for an online UPS (fifth Q1 bit = 0) it multiplies
the raw value by "number of batteries × cells per battery" from its own
settings.

### What the UPS does not report

Battery charge in %, remaining runtime, output frequency, output current, and
battery temperature are absent from Q1. The KSTAR card "computes" charge and
runtime from the voltage and its own tables; the gateway deliberately does not
do this: an estimate without calibration for the specific batteries can make
SNMP clients (PowerChute, NUT) shut down the servers too early or too late.
The gateway instead asks the UPS itself with the read-only `BT`/`BP` queries
(see below); the owner's Sinus EVO 3000 answered both, and the status showed
battery charge 100 % and runtime 2820 s (47 minutes) at 13–14 % load, mains
present. If a UPS does not answer them, these fields remain "no data".

## Management

All commands are available only with "UPS controls" enabled. Commands that
change the output additionally require "Power controls" and `POWER`
confirmation; the rest require `EXECUTE` confirmation. The gateway sends a
command only with fresh Q1 status and no more than one command at a time.

| API command | Bytes | Parameters | Class |
| --- | --- | --- | --- |
| `beeper_toggle` | `Q\r` | toggles the beeper (on/off, not "set") | EXECUTE |
| `test_short` | `T\r` | battery test ≈10 s | EXECUTE |
| `test_deep` | `TL\r` | test until battery discharge | EXECUTE |
| `test_timed` | `Tnn\r` | test for `delay_minutes` = 1…99 minutes | EXECUTE |
| `test_cancel` | `CT\r` | cancel test | EXECUTE |
| `output_off` | `Snn\r` or `S.n\r` | output off via `delay_minutes` = 1…10 minutes or `delay_seconds` = 12, 18, … 54 (n = seconds / 6) | POWER |
| `output_restart` | `SnnRmmmm\r` or `S.nRmmmm\r` | same off delay and on after `restore_minutes` = 3…9999 minutes | POWER |
| `shutdown_cancel` | `C\r` | cancel shutdown | POWER |

`delay_minutes` and `delay_seconds` are not allowed in one request. Seconds are
accepted only as multiples of 6: the KSTAR card rounds down, the gateway instead
refuses, so the real delay matches the requested one.

The same commands can be run on a schedule (Schedules page), including
`test_deep` and `test_timed` (duration is the `delay_minutes` field). That is
how the gateway repeats the KSTAR card's "scheduled self-test". The off delay in
schedules is only in minutes.

Example `POST /api/command` body (requires a session with Operator role or
higher and the `X-CSRF-Token` header from `/api/login`):

```json
{"command":"test_timed","confirm":"EXECUTE","delay_minutes":5}
{"command":"output_off","confirm":"POWER","delay_seconds":30}
```

### SNMP control

The same commands are available over SNMP SET through standard APC PowerNet
objects (`upsAdvControlUpsOff`, `upsAdvTestDiagnostics`, etc.), so NUT `snmp-ups`
(`mibs=apc`), PowerChute, and other managers can control them. A separate Write
Community is required (`snmp_write_community`, empty by default — SET is
forbidden) and the same `controls_enabled` / `power_controls_enabled`
permissions. Delays are fixed: immediate off is 12 s (`S.2`), graceful is 1 min,
on is after 3 min. `noError` on SET means "command accepted into queue", as in
the web interface. Whether the UPS stays off after grid return (`shutdown.stayoff`
in NUT) has not been verified on Sinus EVO. The table of objects, values, and
error codes is in [OIDS.md](OIDS.md#ups-management-over-snmp).

The KSTAR card sends to the same SNMP commands: reboot → `S.2R0000`, sleep →
`S.2R<min>`, flash-and-beep → `Q`. The gateway repeats this, except `R0000`
(see below).

Not implemented and why:
- **`R\r`** ("UPS reboot" in the card) — there is not a single call in the card
  firmware; the card's SNMP "reboot" command actually sends `S.2R0000`.
- **`R0000`** (immediate on) — the meaning of value 0 is not established; the
  gateway requires ≥ 3 minutes.

## Configuration

### What is configured in the UPS itself

Over RS-232 — nothing. There is not a single command in the KSTAR card firmware
that writes UPS settings (output voltage and frequency, ECO/bypass, charge
current, number of batteries): of the 35 command-queueing locations, 25 are
polls and 10 are the management commands above. The Sinus EVO manual
(INF511-Y01-U819-1-02, appendix 2) describes the DB9 connector and cable, but
not the protocol and not the commands.

Settings from the front panel (LCD menu, manual §7.6):

| Parameter | Values |
| --- | --- |
| Output voltage | 208 / 220 / 230 / 240 V (default 220) |
| ECO and bypass (`bPS`) | on/off; tolerance +5/+10/+15/+25 % (default +25 %), lower limit — likewise |
| Frequency converter (`OPF`) | 50 Hz, 60 Hz, or `IPF` (normal mode) |
| Batteries | number of packs and type; by default 2 kVA — 4 × 12 V (48 V), 3 kVA — 6 × 12 V (72 V), 7/9 Ah |
| Battery self-test (`Abt`) | on/off |

The TEST/MUTE button held longer than 1 s starts a self-test in grid/ECO mode
and silences the beeper in battery mode. There is no setting for charge current
or constant beeper silence in the manual.

### What is configured in the gateway (analogues of the KSTAR card settings)

| KSTAR card | Gateway |
| --- | --- |
| number of batteries × cells (battNum, battSingle) | `battery_multiplier` |
| communication timeout (commTimeout) | `stale_ms`, `response_timeout_ms` |
| load, temperature, input voltage thresholds | `alarm_load_high`, `alarm_temperature_high`, `alarm_input_low/high` |
| alarm confirmation time (alarmActTime) | `alarm_delay_s` |
| data recording period (dataLogPeriod) | `history_interval_s` |
| scheduled self-test, scheduled on/off | Schedules |
| protocol and port speed | `driver`, `baud` |

The card has output voltage, battery voltage, and remaining time thresholds;
the gateway does not; the UPS reports runtime only through `BT`, not in Q1.

## KSTAR card extended profiles

The card has profiles with additional requests: "Megatec-1-1-EXT" (`BT`, `BP`,
`NQU`, `A1`: runtime, charge, serial number, alarm words), "Inform Pyramid DSP",
and "Inform Forte" (`Q2`, `SEC`, `BYB`, `INN`, `ALR`, `BTT`: three-phase
measurements and 48 alarm bits). The owner's Sinus EVO 3000 answered the
read-only `BT` and `BP` requests; no source shows that it answers `NQU`, `A1`
or the three-phase Inform requests, and the `ALR` alarm labels describe
three-phase models.
The gateway sends only `BT` and `BP`, both read-only, and disables each one
after three unanswered attempts; see [DRIVERS.md](DRIVERS.md). It does not send
`NQU`, `A1` or the Inform requests. If you need to check whether your UPS
answers them, send them manually through a terminal at 2400 8N1 and save the
responses — they can be used to add support.

## Device verification

1. Enable `controls_enabled`, run `test_short`: the "test in progress" bit
   should appear in Q1, then disappear.
2. `test_timed` with 1 minute: the test bit holds for about a minute;
   `test_cancel` clears it earlier.
3. Run `test_deep` only with a fully charged battery and non-critical load.
4. Measure the battery pack voltage with a multimeter and compare with
   `raw × battery_multiplier`.
