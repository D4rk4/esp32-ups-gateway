# Porting the APC Interface and ESP32 API — 0.2.0

## What the interface carries over

The working interface carries over the main tabs Home / Device Manager / Logs /
Administration, the side navigation, the 750 px shell width, the 140 px side
menu, Verdana, the blue-grey palette, the table styling, and the two-step
command confirmation.

**The load gauge** is based on the original APC `#gauge`: a narrow vertical
scale with 20 raised segments and a large number beside it. The value shown is
the real UPS load percentage. Current, power in watts, and the PDU breaker
rating are not invented. Green means below 80% of the configured threshold,
yellow means approaching the threshold, and red means the value is at the
threshold or above. This is a visual indication; the active alarm separately
accounts for the exceedance and the configured delay. The threshold is taken
from the active `status.thresholds.alarm_load_high` for all roles. When the
value is unavailable, the segments are inactive and the number is replaced with
a dash. Age is counted from `age_ms` and the time since the response, and the
limit is taken from the active `ups.stale_ms`; a separate timer clears the
indication even during a hung HTTP request. Load above 100% is kept as a number;
the gauge fill is limited to its top edge.

The new server and client were written for the shared UPS Gateway model. The
handlers of the original APC `/Forms/...` are not executed on the ESP32. The
device name in the interface is UPS Gateway; it does not claim to be the
original APC board.

## Page and function correspondence

| APC scenario | ESP32 implementation | Data / action |
| --- | --- | --- |
| Home / Overview, Load Status | UPS overview, load gauge, parameters, and recent events | `GET /api/status`, `/api/alarms`, `/api/events` |
| Alarm Status / Load thresholds | Current hardware flags and threshold alarms | `/api/alarms`; thresholds via `/api/config` |
| Device Status | General metrics, availability, Q1 diagnostic line | `/api/status` |
| Outlet Control / Confirmation | UPS commands by driver capabilities and a separate confirmation screen | `/api/capabilities`, `POST /api/command` |
| Scheduling | Up to eight one-shot/weekly rules | `/api/schedules`, `schedules` config |
| Event Log | Filter by level/text, CSV, explicit clear | `/api/events`, `/api/events.csv`, `/api/logs/clear` |
| Data Log / Interval | 240-point history in RAM, chart, CSV, interval | `/api/history`, `/api/history.csv`, `history_interval_s` |
| Administrator / Device User / Read-only | Three roles, server-side restrictions, inactivity timeout | `/api/login`, `/api/session`, `/api/logout` |
| TCP/IP / DNS | DHCP or static IPv4, gateway, and DNS | `/api/config` |
| SNMP Access | SNMPv2c read community and NMS IPv4 restriction | `/api/config` |
| Trap Receivers / Test | Two receivers, port/community, delivery test into the network stack | `/api/config`, `/api/trap-test` |
| Syslog / Test | UDP syslog, level, server/port, separate queue | `/api/config`, `/api/notifications`, `/api/syslog-test` |
| Identification | Name, contact, location | `/api/config` |
| Date & Time / NTP / timezone | NTP or manual UTC, POSIX TZ | `/api/time`, `/api/config` |
| Preferences / Quick Links | Date format, °C/°F, start page, three links | `/api/config` |
| Configuration backup / restore | Explicit secret JSON export and validated import | `/api/config/export`, `POST /api/config` |
| Reset / Reboot | Reset of functional settings with access preserved; gateway restart | `/api/reset-config`, `/api/reboot` |
| About / diagnostics | Version, flash/RAM, network, UART, config schema, capabilities | `/api/about`, `/api/capabilities` |

## API and authorization

The root `/` serves the shell with the login form. `POST /api/login` accepts
`{"username":"admin","password":"..."}` and creates a random session in memory.
Cookie `ups_session` has `HttpOnly`, `SameSite=Strict` and `Path=/`.
`GET /api/session` without login returns `authenticated:false`; after login it
adds the role, CSRF, boot_id, version, capabilities, and display parameters.

Mutating requests after login require `X-CSRF-Token` from the current session.
Responses are not cached. Basic auth from version 0.1.0 was removed: an external
HTTP client needs to log in and keep the cookie. SNMP polling by Observium does
not depend on this.

| Role | Read state/logs | UPS commands | Configuration, secret export, time, reset, restart |
| --- | --- | --- | --- |
| Read-Only User | Yes | No | No |
| Device User | Yes | Yes, with other restrictions | No |
| Administrator | Yes | Yes, with other restrictions | Yes |

Up to six active sessions; the login failure table is also limited. Ordinary
background GETs do not extend the session. UI marks real human actions with
the `X-Session-Activity: 1` header; the server checks role/CSRF independently
of the state of form elements. Passwords are not saved by the browser in
localStorage/sessionStorage. HTTP does not encrypt traffic; version 0.2.0 is
intended for a trusted management network.

Configuration is passed as a **partial object**: saving one page does not reset
other fields. An empty secret means keeping the old value. Ordinary `GET
/api/config` returns empty secrets and `*_set` flags; the export includes real
secrets. Changes take effect after a restart; current readings use the active
configuration copy. The limit of the `/api/config` body is 12288 bytes; of
other POSTs — 2048 bytes.

When saving a new/changed enabled power schedule, a separate `POWER` entry is
required; confirmation is passed through the temporary `confirm` field and is
not written to the configuration. The same rule applies to changes that
activate a saved rule, and to import. The content of the import file itself is
not considered confirmation.

## Configuration persistence and schedule execution

Historically version 0.2.0 used one `factory` at 0x10000 of size 0x3F0000 and
`appcfg` at 0xF00000 of size 0x10000. The current OTA layout, the one-time USB
transition, and update rules are described in [OTA.md](OTA.md). The address and
size of `appcfg` are unchanged. The UI is embedded in the application; the
`logs` partition is only reserved and is not a filesystem.

Config schema 1 migrates to schema 2 in memory while keeping previous secrets.
There is no automatic NVS erasure on error. The scheduler stores execution
attempt markers in a separate namespace of the same appcfg. It saves a marker
before queuing the command, does not repeat it after an undefined result, and
does not execute skips retroactively. On restart/time jump, a protection window
applies. Availability of a valid time does not confirm physical command
execution; the `sent_unconfirmed` state means only that bytes were transferred.
The exact supported UTC window is returned by `/api/time`. In the fixed Arduino
ESP32 2.0.17 / toolchain of this release, `time_t` is 32 bits: the upper limit
is January 19, 2038, 03:14:07 UTC. The UI reads this limit from the API.

## Porting boundaries

This version lacks SMTP/email, HTTPS/certificate management, SNMPv1/v3,
INFORM, RADIUS, SSH/Telnet, FTP/SCP, WAP, and WoL. The corresponding APC
services cannot be enabled with a non-working switch. The capabilities section
lists the unsupported services. The full matrix of individual actions for each
APC event, journal rotation to FTP, card group synchronization, and the model
of independent PDU outlets/banks are not implemented. The gateway provides
common thresholds, syslog filtering, and two trap receivers.

These are the explicit boundaries of this release, not a claim that all APC
network card services were ported. New output groups and another UPS protocol
need to be added through extending the shared model and driver, then API/UI.

## How to verify the UI

`tests/ui_fixture.html` is a standalone stand with synthetic API responses and
real client code `web/index.html`. It is not part of the embedded web resource
of the firmware, does not access the ESP32, and explicitly marks test data.
The stand switches create normal/high load, RS232 loss, HTTP error, and session
termination. Such a stand allows checking client scenarios, but does not
replace hardware verification.

Final build, test, and browser check availability results are described in
`BUILD_REPORT.md`. Screenshots may be considered verification only after the
page is actually displayed in a browser.
