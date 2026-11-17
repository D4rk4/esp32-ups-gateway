# Release 0.1.0 verification — 11 November 2026

## What was actually done

The project was built with the standard `tools/build.py` for `esp32-poe-16mb`.
This is cross-compilation for ESP32, not only a syntax check or code stub.
Sources from `src/`, `lib/` and `web/` match the produced images; they were not
changed after the final build.

| Parameter | Actual result |
| --- | --- |
| Target | ESP32-POE WROOM, 16 MB flash, MOD-RS232 |
| PlatformIO Core | 6.1.18 |
| Platform | espressif32 6.12.0 |
| Framework | Arduino ESP32 2.0.17 |
| ESP32 compiler | Xtensa GCC 8.4.0+2021r2-patch5 |
| ArduinoJson | 6.21.5 |
| App size per linker report | 901125 bytes / 4128768, 21.8% |
| Statically allocated RAM | 48364 bytes / 327680, 14.8% |
| Ready firmware.bin including image format | 907696 bytes |
| Web HTML/JS/CSS | 50288 bytes, built-in gzip 15076 bytes |

Static RAM in the table does not include all dynamic allocations during
operation. The real minimum free heap and long-term stability must be measured
on the device.

## Images and layout

Image headers, sizes, the fixed partition table, and checksums were verified.
The bootloader and application have flash size code 4, corresponding to 16 MB.
The table contains a single `factory` slot and a separate NVS `appcfg`; there
are no OTA partitions. A `tools/flash.py --dry-run install` check passed
without accessing the serial port.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| bootloader.bin | 17536 | a23f2db3a09e9df581a397ade210cce2224b593f8ae563a28f75b5b9795f30b3 |
| partitions.bin | 3072 | 6994764429c1125ab6c8b08276f9785ac9b85dcb27a06107a04c1a80c0b966fb |
| firmware.bin | 907696 | 5430a4e53e1580889aec9e8db9c470f4408da3e94b3da8596d37a935523efc12 |

The same information is in `firmware/build-info.json` and
`firmware/SHA256SUMS.txt`. The build script updates them on rebuild.

## Automatic checks

| Check | Result and boundaries |
| --- | --- |
| `python tools/test_host.py` | All 6 C++ suites passed; strict compiler warnings enabled |
| `ASAN_OPTIONS=detect_leaks=0 python tools/test_host.py --sanitize` | The same 6 suites passed with AddressSanitizer and UndefinedBehaviorSanitizer |
| SNMP inside these suites | 16 groups: BER, OID, GET/GETNEXT/GETBULK, errors, read-only SET, traps and size constraints |
| `python -m unittest discover -s tests -p 'test_*.py' -v` | All 28 tests passed: 14 flash-helper and 14 trap-adapter |
| JavaScript `node --check` | Embedded code `web/index.html` is syntactically correct |
| `git diff --check` | No diff formatting errors found |

C++ suites cover strict configuration validation and JSON import,
secret preservation with empty fields, refusal without partial modification,
MegaTec Q1 driver, state transitions, PowerNet and SNMP conformance.
Firmware script tests use writing stub transport calls: they check write
addresses, image limits, flash size, configuration copy and comparison,
refusal before writing under invalid conditions. Physical board these tests do
not program.

During independent review, a case was found and fixed where a large GETBULK
response lost all results. Re-running the original reproducer with real
`PowerNetMib` returned 1398 bytes, status 0 and 53 varbinds: an ordered prefix
fitting the limit is preserved. Also fixed UART baud high-speed round-trip,
form UTF-8 byte limits, and the `sent_unconfirmed` signature. In the address
repeated review of remaining remarks, no P0/P1/P2 level issues were found.
This is a source review, not a security certification.

## Environment limitations and unverified checks

- The initial sanitizer run with LeakSanitizer ended with a message about LSan
  incompatibility with ptrace in this environment. The final 6 suites were run
  with `detect_leaks=0`: ASan and UBSan are enabled; LSan leak checking is not
  claimed.
- Browser verification was not completed: local Chromium could not create system
  sockets due to environment restrictions. JS syntax and form/API compliance
  were verified, but visual rendering and browser scenario are not declared
  tested. `tests/web_ui_check.py` — optional scenario with synthetic API for
  separate run with Playwright; its successful execution in this release is not
  claimed.
- No real ESP32-POE, MOD-RS232, or Inform Sinus EVO here. Not verified: board
  flashing, electrical exchange, semantics of management commands of a specific
  UPS, Ethernet under load, NVS preservation after physical reflashing/power
  interruption and long-term operation.
- PHP, snmptrapd, rsyslogd and a running Observium CE server are absent here.
  Prepared profile and integration configurations; adapter tests on synthetic
  inputs were performed. Receipt of a real trap and discovery sensors of a
  specific CE version were not verified.

During build, unwanted PlatformIO access to the telemetry server was detected.
It is disabled by the supported setting; `tools/build.py` also explicitly
disables telemetry. Loading required packages at the first build requires
internet.

## Primary stand acceptance

1. Flash the board per README and verify USB console, Ethernet address, and web
   interface login.
2. Verify that Q1 gives stable answers, and compare readings with the UPS.
3. Check SNMP walk, discovery in CE, test trap, RS232 break and restoration.
4. Change one harmless setting, save it, run the standard `update` and verify
   that the setting is preserved; the script additionally compares raw NVS.
5. Only after protocol confirmation and on a suitable load, allow control.
   Transmitting a command over UART is marked as unconfirmed; the load state is
   checked separately.

All sources, web resources, tests, PlatformIO configuration and scripts are
included in the ZIP together with ready images. PlatformIO dependencies are
installed at build by pinned versions.
