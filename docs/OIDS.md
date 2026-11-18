# Implemented SNMP interface

SNMPv2c, UDP 161. GET, GETNEXT and GETBULK are supported. SET is accepted only
with a separate Write Community and only for UPS management objects
(see "UPS management over SNMP"); with Read Community SET returns
`notWritable`. Maximum UDP packet 1400 bytes, input varbinds 32, output 64.
A large GETBULK is shortened to the fitting ordered prefix. A large ordinary
GET/GETNEXT receives `tooBig`.

## Gateway identification

| OID | Name | Type / value |
| --- | --- | --- |
| 1.3.6.1.2.1.1.1.0 | sysDescr | `ESP32 UPS Gateway/0.2.2; driver=...; PowerNet subset` |
| 1.3.6.1.2.1.1.2.0 | sysObjectID | OBJECT IDENTIFIER `0.0`, vendor identifier not yet assigned |
| 1.3.6.1.2.1.1.3.0 | sysUpTime | TimeTicks, hundredths of a second since startup |
| 1.3.6.1.2.1.1.4.0 | sysContact | String from configuration |
| 1.3.6.1.2.1.1.5.0 | sysName | Gateway name |
| 1.3.6.1.2.1.1.6.0 | sysLocation | Location |
| 1.3.6.1.2.1.1.7.0 | sysServices | INTEGER 72 |

For Observium use the attached definition by `sysDescr`. Presence of an APC
OID does not mean that the gateway is a specific APC model.

## PowerNet

Add the prefix `1.3.6.1.4.1.318.1.1.1.` to each suffix below.

| Suffix with .0 | Object | Type | Unit / condition |
| --- | --- | --- | --- |
| 1.1.1.0 | upsBasicIdentModel | OCTET STRING | Model from I; otherwise driver designation |
| 1.1.2.0 | upsBasicIdentName | OCTET STRING | Gateway name |
| 1.2.1.0 | upsAdvIdentFirmwareRevision | OCTET STRING | UPS firmware version from I, if received |
| 1.2.3.0 | upsAdvIdentSerialNumber | OCTET STRING | Only if the driver provided a serial number |
| 8.1.0 | upsCommStatus | INTEGER | 1 connection present; 2 connection absent |
| 2.1.1.0 | upsBasicBatteryStatus | INTEGER | 1 unknown; 2 no low-battery sign; 3 low battery |
| 4.1.1.0 | upsBasicOutputStatus | INTEGER | 1 unknown or stale; 2 online; 3 on battery; 9 bypass; 15 battery test (self-test with utility present) |
| 3.2.1.0 | upsAdvInputLineVoltage | Gauge32 | Volts |
| 3.3.1.0 | upsHighPrecInputLineVoltage | Gauge32 | Tenths of a volt |
| 3.2.4.0 | upsAdvInputFrequency | Gauge32 | Hertz |
| 3.3.4.0 | upsHighPrecInputFrequency | Gauge32 | Tenths of a hertz |
| 4.2.1.0 | upsAdvOutputVoltage | Gauge32 | Volts |
| 4.3.1.0 | upsHighPrecOutputVoltage | Gauge32 | Tenths of a volt |
| 4.2.3.0 | upsAdvOutputLoad | Gauge32 | Percent |
| 4.3.3.0 | upsHighPrecOutputLoad | Gauge32 | Tenths of a percent |
| 2.2.2.0 | upsAdvBatteryTemperature | Gauge32 | Internal UPS temperature, °C; below 0 → 0 |
| 2.3.2.0 | upsHighPrecBatteryTemperature | Gauge32 | Same temperature, tenths of °C |
| 2.2.8.0 | upsAdvBatteryActualVoltage | INTEGER | Battery chain voltage, only with multiplier configured |
| 2.3.4.0 | upsHighPrecBatteryActualVoltage | INTEGER | Tenths of a volt of battery chain |

The names of the temperature objects historically contain Battery, but their
description in PowerNet refers to **internal UPS temperature**. In the web
interface it is UPS temperature, not a separate battery sensor. The bypass=9
code is a compatible representation of detected bypass; Q1 does not report the
reason it was turned on.

`4.1.1.0` is derived from the driver's output mode only, in this order: utility
failure → 3; a running battery test → 15; detected bypass → 9; otherwise 2
(1 while the status is unknown or stale). A real outage therefore always wins
over a test: when the Q1 test bit is set together with utility failure, the
object reports 3. Firmware 0.2.1 and earlier reported 15 whenever the test bit
was set, even with utility failed.

`2.2.1.0` — charge (%, Gauge32) and `2.2.3.0` — runtime (TimeTicks) are present
only after the UPS has answered the read-only `BT` (runtime) and `BP` (charge)
queries, which the gateway repeats about every 30 seconds. A UPS that does not
answer them leaves the objects absent (three consecutive failures disable that
query until restart), and both objects are absent while the RS232 link is lost.
Measured output frequency, `4.2.2.0` / `4.3.2.0` (Hz / 0.1 Hz), is provided by
the adapter for future drivers but is not supplied by the Q1 driver.

Unknown/unsupported measurements are absent: GET gets `noSuchInstance`, walk
skips them. On RS232 loss measurements also stop being reported as current;
statuses become unknown/noComm. Model/version may persist after loss of
connection. Last values are available in the web interface with an explicit
staleness flag.

## UPS management over SNMP

Disabled by default. Enabled by setting the Write Community (Network / SNMP
page, field `snmp_write_community`, 1–64 bytes, must differ from Read
Community). In addition, the same permissions apply as for web management:
`controls_enabled` for all commands and `power_controls_enabled` for commands
that change the output. The `snmp_manager_ip` restriction also applies to SET.
A request with Write Community may do both GET and SET.

| Suffix with .0 | Object | Value → MegaTec command |
| --- | --- | --- |
| 6.2.1.0 | upsAdvControlUpsOff | 2 → `output_off` after 12 s (`S.2`); 3 (graceful) → after 1 min (`S01`) |
| 6.1.1.0 | upsBasicControlConserveBattery | 2 → `output_restart`: off after 1 min, on after 3 min (`S01R0003`); only on battery |
| 6.2.2.0 | upsAdvControlRebootShutdownUps | 2 → `output_restart` after 12 s, on after 3 min (`S.2R0003`); 3 → after 1 min (`S01R0003`) |
| 6.2.5.0 | upsAdvControlFlashAndBeep | 2 → `beeper_toggle` (`Q`, toggles sound) |
| 6.2.6.0 | upsAdvControlTurnOnUPS | 2 → `shutdown_cancel` (`C`) |
| 7.2.2.0 | upsAdvTestDiagnostics | 2 → `test_short` (`T`) |
| 7.2.5.0 | upsAdvTestRuntimeCalibration | 2 → `test_deep` (`TL`); 3 → `test_cancel` (`CT`) |
| 5.2.10.0 | upsAdvConfigShutoffDelay | TimeTicks 6000 (60 s — graceful shutdown delay), read-only |
| 5.2.9.0 | upsAdvConfigReturnDelay | TimeTicks 18000 (180 s — turn-on delay), read-only |

What the UPS does after `S01` and `SnnRmmmm` with network absent and restored
(whether it remains off, whether `mmmm` counts from disconnection or from
network return) is not verified on Sinus EVO. In particular, NUT `blazer`/`nutdrv_qx`
for `shutdown.stayoff` over RS-232 sends `SnnR0000`, while the gateway on
`upsAdvControlUpsOff = 3` — `S01`; whether the UPS remains off after network
return is not guaranteed. Verify on your UPS before using it in automatic
shutdown scenarios.

GET on a management object returns INTEGER 1 ("no action", as PowerNet-MIB
requires), but the object is visible (with any community) only if Write
Community is set, needed permissions are enabled, and the driver supports the
command. Otherwise GET gets `noSuchInstance`, and walk skips it. NUT
`snmp-ups` (`mibs=apc`) declares a command only on successful GET, so
`upscmd -l` shows exactly the available commands:

| NUT | Object and value |
| --- | --- |
| `load.off` | upsAdvControlUpsOff = 2 |
| `shutdown.stayoff` | upsAdvControlUpsOff = 3 |
| `shutdown.return` | upsBasicControlConserveBattery = 2 |
| `load.on` | upsAdvControlTurnOnUPS = 2 |
| `test.panel.start` | upsAdvControlFlashAndBeep = 2 |
| `test.battery.start` | upsAdvTestDiagnostics = 2 |
| `calibrate.start` / `calibrate.stop` | upsAdvTestRuntimeCalibration = 2 / 3 |

SET responses (SNMPv2c error-status):

| Situation | Response |
| --- | --- |
| Value 1 | `noError`, command not sent |
| Command accepted into queue | `noError`. This is only queue acceptance: if RS232 sending later fails (status stale, UART write incomplete), SET will not report it — the error goes to the event journal and `last_state` on the Control page. Verify UPS execution on the next Q1 |
| Byte-for-byte repetition of the last SET from the same address and port within 10 s | Same answer from cache; the command is not queued again. Cache for one request: if another non-empty SET with Write Community (from any manager) arrives between the original SET and the repetition, the repetition is executed again |
| Empty varbind list | `noError` |
| Read Community, unknown or read-only OID | `notWritable` (17) |
| Not INTEGER (on a writable object) | `wrongType` (7) |
| Another value; ConserveBattery not on battery | `wrongValue` (10) |
| More than one action in the request | `inconsistentValue` (12) on the second, nothing is sent |
| Control disabled, command not supported, Q1 status stale, UPS busy with another command | `inconsistentValue` (12) |

The error is returned for the first invalid varbind in order. The request is
checked as a whole before queueing the command.

SNMPv2c passes community in plain text. Write Community must not equal either
Read Community or the community of the enabled trap receiver (otherwise the trap
would expose it to its recipient). Limit SET via `snmp_manager_ip` and keep the
gateway in a trusted management network. Delays of 12 s, 1 min, and 3 min are
chosen to encode exactly in MegaTec without rounding; no configurable delays
over SNMP. The KSTAR card sends `S.2R0000` on "reboot"; the gateway does not
use `R0000` because the meaning of zero is not established.

## Notifications

SNMPv2c TRAP only. INFORM, SNMPv1 and SNMPv3 are not implemented. In each
trap the first varbinds are sysUpTime.0 and snmpTrapOID.0, then
`mtrapargsString.0` (`1.3.6.1.4.1.318.2.3.3.0`) and sysName.0.

| Trap OID | Event |
| --- | --- |
| 1.3.6.1.4.1.318.0.1 | communicationLost |
| 1.3.6.1.4.1.318.0.8 | communicationEstablished |
| 1.3.6.1.4.1.318.0.5 | upsOnBattery |
| 1.3.6.1.4.1.318.0.9 | powerRestored |
| 1.3.6.1.4.1.318.0.7 | lowBattery |
| 1.3.6.1.4.1.318.0.11 | returnFromLowBattery |
| 1.3.6.1.6.3.1.1.5.1 | coldStart: gateway start or explicitly marked channel check |

Two receivers, each with IPv4/port/community. Queue of 16 events. Without
Ethernet events wait no more than 30 seconds before sending resumes; older
entries are not sent as new ones. The circular 48-record journal remains in
RAM until restart. `sent` means successful transmission of the UDP packet to
the local stack, not acknowledgment of receipt.

General Q1 fault, test start/end, and the shutdown procedure go to the local
journal. They are not substituted by other APC events such as overload,
battery replacement, or test result, which Q1 does not report. A battery test
while utility power is present sends no upsOnBattery trap and writes no
on-battery journal event.

## Hardware observation

2026-11-18, one Olimex ESP32-POE 16 MB board, firmware 0.2.1, Inform Sinus EVO
3000. With the read community, SNMPv2c GET, a GETNEXT walk and a GETBULK walk
returned the 7 system objects and 22 PowerNet objects; the bulk walk listed the
same OIDs as the GETNEXT walk and ended with endOfMibView; values matched the
web status taken at the same moment; absent objects returned noSuchInstance; a
wrong community and an SNMPv1 request got no answer. One SET was also verified:
with a write community configured and controls and power controls enabled,
upsAdvTestRuntimeCalibration (`7.2.5.0`) = 3 during a running deep test returned
noError, the journal recorded `test_cancel` queued, transmitted and "Test
ended", and upsBasicOutputStatus went from 15 to 2 within 5 s; before that SET,
GET of `7.2.5.0` with the write community returned 1. On 0.2.1 the other control objects
and traps were not exercised on hardware.

2026-11-18, the same board on firmware 0.2.2. An SNMPv2c SET of
upsAdvTestDiagnostics (`7.2.2.0`) = 2 returned noError and started a short
test: the journal recorded `test_short` queued, transmitted, "UPS test in
progress" 1 s later and "Test ended" 11 s after the start, and
upsBasicOutputStatus read 15 (battery test with utility present) from about
1 s to 11 s after the SET and then 2 again, with the load not interrupted. A
timed test started and cancelled from the web UI also read 15 while it ran and 2
again when `test_cancel` was sent. The remaining control objects and traps have
not been exercised on hardware.
