# Release 0.2.0 verification — 11 November 2026

**Historical report:** the information below refers to the original build of
11 November 2026, not to the currently rebuilt files in `firmware/`.

**Current build state on macOS (12 November 2026):** `firmware.bin` contains
1 035 008 bytes, SHA-256:
`0ceedeba1b1707179ecfad51489c0191cc2173e36c06acbecdd8b9664d579d33`.
All 10 native sets passed, including with ASan/UBSan. The HTTP adapter passed in
both modes; also passed were 90 Python tests, the EventBus check, and the UI
contract. At the time of this writing, a rebuild on physical hardware was not
verified.

`python tools/package_release.py` exits with code 1 and creates no archive:
Semgrep — PASS on the narrow local rule set (this is not a security audit);
Trivy — FAIL: no vulnerabilities, secrets, or configuration errors, but there
are two findings on build tool licenses from `requirements.txt` (esptool —
GPL-2.0-or-later, platformio — Apache-2.0), and PlatformIO dependencies are not
covered by Trivy. Coverage 100%, crapper, dryer, and mutator — BLOCKED,
because C/C++, embedded JavaScript, and PHP are not covered by these tools,
and Python code does not reach the thresholds. Release readiness is not claimed.

## Result and boundaries

The firmware was indeed cross-compiled for ESP32-POE WROOM 16 MB and MOD-RS232
via the standard `tools/build.py`. The final build was made after fixing
sessions, out-of-memory handling, and load gauge freshness. Firmware and UI
sources were not changed after this build; documentation and packaging are
separate.

**No screenshots were obtained.** The managed browser declined opening the local
`file://` stand by URL policy: only HTTP/HTTPS are allowed; bypass via another
browser/surface was also rejected. The attempt was stopped. Neither browser
rendering nor real clicks in the browser were verified. Mockups and
programmatically drawn images instead of screenshots were not created. A user
request to validate screenshots remains unfulfilled.

A separate `tests/ui_fixture.html` contains the real client code and explicitly
marked synthetic API for local viewing. The test handler does not access the
network/UPS and is not embedded in the firmware. Matching of client JavaScript
between the stand and the production HTML was verified byte-by-byte. Unpacked
gzip content of `src/web_assets.hpp` was also verified byte-by-byte.

## Actual build

| Parameter | Result |
| --- | --- |
| Version | 0.2.0 |
| Board | Olimex ESP32-POE WROOM, physical 16 MB flash, MOD-RS232 |
| PlatformIO Core | 6.1.18 |
| Platform | espressif32 6.12.0 |
| Framework | Arduino ESP32 2.0.17 |
| ESP32 compiler | Xtensa GCC 8.4.0+2021r2-patch5 |
| ArduinoJson | 6.21.5 |
| App per linker | 1027381 / 4128768 bytes, 24.9% |
| Static RAM | 50948 / 327680 bytes, 15.5% |
| Ready firmware.bin | 1033952 bytes |
| Production HTML/JS/CSS | 127264 bytes |
| Built-in gzip UI | 41963 bytes |
| App slots | Single factory: 0x10000 / 0x3F0000 |
| Configuration | Previous appcfg: 0xF00000 / 0x10000 |

Static RAM does not include all dynamic allocations, task stacks, TCP, and NVS.
Minimum free heap and stack margin on the real ESP32 were not measured.
PlatformIO telemetry is disabled. A separate filesystem and an OTA slot are not
needed.

## Tests performed

| Check | Actual result |
| --- | --- |
| 10 native suites of shared C++ modules | All passed |
| The same 10 suites with ASan/UBSan | All passed, no ASan/UBSan diagnostics |
| SNMP BER/GET/GETNEXT/GETBULK | 16 groups in the native suite passed |
| Python: install/update/backup and trap→syslog | 28 tests passed |
| Real WebService handlers with substituted ESP/HTTP boundary | Native and ASan/UBSan passed |
| Real EventBus with in-memory queue | ASan/UBSan passed; log cleanup preserves trap queue |
| Current production JavaScript in Node VM | Contract/state checks passed |
| `node --check tests/ui_fixture_api.js` | Syntax verified |
| Web gzip packaging and stand content | Client app content matches |
| Verification of new flash images and dry-run update | Headers, sizes, table, and SHA-256 verified; no hardware write |
| Independent limited review | Found UI freshness defect fixed and rechecked |

Native suite: auth, config, config_json, config_v02, driver, events,
http_contract, management, powernet, snmp. Compilation uses
`-Wall -Wextra -Wpedantic -Werror`. LeakSanitizer is disabled only due to the
ptrace limitation of this environment (`ASAN_OPTIONS=detect_leaks=0`); ASan and
UBSan are enabled. Success of these tests does not prove absence of leaks on
the device.

Substantial verified cases:

- Full schema 1 migration to schema 2 with exact secrets preserved; rejection of
  invalid/future format without substituting the original configuration.
- JSON/field limits, empty secret preservation, and atomic refusal of an
  erroneous edit; separate roles, CSRF, sessions, and login attempt limits.
- Session expiry between check and processing does not permit read/write.
  Memory allocation errors do not cause response recursion and are not issued
  as empty 200.
- Fresh `POWER` required before saving new/changed power rules, including
  enabling saved rules, driver change, and weekly rule time changes.
- The scheduler fixes claim before enqueue; a write error prohibits sending.
  No catch-up, repeats after restart/clock jump, or double weekly execution on
  repeated DST hour. A dubious result remains `uncertain`.
- An ordinary simple weekly rule per 24 hours requires one startup record, not
  an NVS write every minute. Execution/skip markers are stored separately.
- History limited to 240 points; missing and stale numbers remain null.
  Threshold events are not masked under physical APC alarm traps.
- APC gauge has 20 segments; verified normal, yellow, red, missing, and stale
  load, active threshold, and rights/command limits.
- UI accounts for `age_ms` plus time after response, active `ups.stale_ms`, and
  up to 15 seconds cache maximum. A separate timer clears the indication even
  during a hung request. Verified 700 ms, 10000 ms, exact boundary, incorrect
  clock, and values.
- Form saving sends only its fields, leaves other settings, and separates
  current/saved configurations. Import does not substitute POWER from the file.

Node VM uses a minimal document model. It verifies client functions and API
calls, but does not perform CSS layout, real browser DOM, cookie/CORS/TCP, and
mouse interaction. HTTP adapter uses real handlers and shared modules, but
substitutes transport, ESP32, and peripherals.

## Images and checksums

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `bootloader.bin` | 17536 | `a23f2db3a09e9df581a397ade210cce2224b593f8ae563a28f75b5b9795f30b3` |
| `partitions.bin` | 3072 | `6994764429c1125ab6c8b08276f9785ac9b85dcb27a06107a04c1a80c0b966fb` |
| `firmware.bin` | 1033952 | `534ff0be2a72adaf19f180d8a42742a32fe7891375b939488e160c633a7e123c` |

SHA-256 `web/index.html`:
`c72b5dbc862b10604fe407c11794ef98371515bf4a24e2d00708e6d66b13b34d`.

## What remains to be verified on hardware

ESP32 was not flashed, Inform Sinus EVO was not physically connected. Not
confirmed: specific cable pinout, actual execution of control commands, trap/syslog
receipt, NTP, live discovery and Observium CE graphs, NVS preservation on real
update/power failure, dynamic memory margin, and long-term operation.
Commands are forbidden by default; `sent_unconfirmed` does not mean UPS ACK.

The package contains adapted main APC scenarios, not all card services:
SMTP, HTTPS, SNMPv1/v3/INFORM, RADIUS, SSH/Telnet, FTP/SCP, WAP, WoL, full
matrix of individual notifications, and independent PDU outlets are not
implemented. Porting map: `APC_PORT.md`; exact UI/API matrix:
`UI_API_COVERAGE.md`. Fixed SDK clock limit — 2038-01-19 03:14:07 UTC; the
limit is returned by the API and checked on time/schedule entry.

Test repetition and installation commands are in `README.md`.
The historical report 0.1.0 is saved in `releases/BUILD_0.1.0.md`.
