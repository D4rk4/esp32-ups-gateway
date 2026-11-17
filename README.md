# ESP32 UPS Gateway 0.2.0

Modular firmware for **Olimex ESP32-POE / ESP32-POE-16MB with WROOM and 16 MB flash + MOD-RS232 on UEXT**. Monitoring via APC PowerNet SNMPv2c, traps, web interface in English, UPS control through a separate UPS driver.

**Verification boundary:** the project is built for ESP32 and tested on a computer. There is no physical ESP32 / Inform Sinus EVO in this environment. For Inform Sinus EVO the MegaTec Q1 driver is supported. Control commands are disabled by default. Monitoring starts automatically.

## What's included

- Ethernet LAN8720, DHCP or static IPv4, no Wi-Fi configuration.
- SNMPv2c GET/GETNEXT/GETBULK, PowerNet MIB subset and UPS link status.
- UPS control via SNMP SET through PowerNet control objects (compatible with NUT `snmp-ups`) with a separate Write Community; disabled by default.
- SNMPv2c traps: battery/mains, low battery/restoration, RS232 loss/restoration, startup and channel test.
- Web interface in classic APC layout: tabs, side menu, compact tables and vertical segment load bar. Resources are embedded in the application and do not require a separate filesystem upload.
- Login by session, administrator/operator/observer roles, logout and idle timeout; control command confirmation on a separate screen.
- Active alarms, configurable thresholds, event log with filter and CSV, 240-measurement RAM history with graph and CSV.
- NTP/manual time, POSIX time zone, one-off and weekly schedules for driver-supported commands, UDP syslog with a separate queue.
- Two trap receivers with separate communities and ports.
- Settings export/import, random SNMP community, web password recovery over USB.
- Two application OTA slots; configuration stored separately and survives a normal update.
- Ready binaries in `firmware/`, full source code, configuration integrity-checking install script.

No full copy of the APC network card or KPM220 is included here: SMTP, WoL, SNMPv3, INFORM, HTTPS, RADIUS, SSH/Telnet, FTP and firmware update over HTTP are not implemented. Update is performed over USB with a single application slot. PDU outlets are not created from nothing: commands and data depend on the UPS driver. Function/API porting map — `docs/APC_PORT.md`. Reference extracted from the user's APC package is included in `reference/apc-webui/`.

## 1. Connecting hardware

During USB flashing **disconnect the Ethernet cable with PoE**. A regular ESP32-POE has no PoE galvanic isolation; this is a direct Olimex requirement. Perform primary flashing with RS232 disconnected, powering the board from USB. MOD-RS232 converts levels but does not add galvanic isolation.

| Purpose | Connection |
| --- | --- |
| ESP32 TX | GPIO4 → UEXT pin 3 → MOD-RS232 |
| ESP32 RX | GPIO36 ← UEXT pin 4 ← MOD-RS232 |
| MOD-RS232 power | UEXT 3.3 V and GND |
| Ethernet | LAN8720, addr 0, MDC23, MDIO18, power12, clock GPIO17 OUT |

With a properly configured MOD-RS232, transmission goes to DB9 pin 2 and reception through pin 3. The official Sinus EVO 1–3 kVA manual also specifies pin 2 TX / pin 3 RX. For this revision you get a crossover cable:

| MOD-RS232 DB9 | Sinus EVO DB9 |
| --- | --- |
| 2 — TX | 3 — RX |
| 3 — RX | 2 — TX |
| 5 — GND | 5 — GND |

Check your UPS connector and revision. Do not carry over the old MegaTec 9/6/7 pinout from the protocol description onto Sinus EVO. This build is not intended for WROVER with a different Ethernet/PSRAM wiring. Details and primary sources: `docs/HARDWARE.md`.

## 2. Quick install of ready firmware

Python 3.10+ and your board's USB-UART driver are needed. Unpack the archive and open a terminal in the `esp32-ups-gateway` directory.

### Windows, PowerShell

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tools\flash.py --port COM5 install
```

Replace COM5 with your port. Venv activation is not required.

### Linux

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/flash.py --port /dev/ttyUSB0 install
```

The user must have access to the selected serial port. If there is a communication problem, you can add `--baud 115200` **before** `install`.

The script checks flash size (16 MB), local images, the new partition table and checksums; saves a raw copy of the configuration in `backups/`; writes bootloader, partition table and application at fixed addresses; resets only the OTA slot selection; reads the configuration area again and compares SHA-256. The chip stays in the bootloader until verification completes, so the first launch does not interfere with comparison. On success a restart is performed. `install` is installation or restore, not a check of old partition table compatibility; to transition from a previous version use `migrate` from section 7. On error check the message; normal operations do not erase the entire flash and do not reset settings automatically.

To view the plan without a board:

```bash
python tools/flash.py --dry-run install
```

### First launch and web interface login

**Important:** `firmware/firmware.bin` is rebuilt from current sources and contains the factory login described below. Verification on a physical board was not performed. On updating, existing username and password are preserved and are not automatically changed.

For a new configuration in firmware built from current sources, the administrator username and password are `ups` / `ups`. These are publicly available weak credentials: immediately after logging in, replace the password with a unique one at least 12 characters long, and use only a trusted management network. An exception to the minimum length is allowed only for the exact factory pair `ups` / `ups`.

Open the USB console at **115200 baud**. For example:

```bash
python -m serial.tools.miniterm /dev/ttyUSB0 115200
```

On Windows use COM5 instead of `/dev/ttyUSB0`; take Python from the created venv. On first launch the web credentials and random SNMP read community are printed. On a configuration read or write error the device uses a temporary random web password and does not replace saved data; these temporary credentials are printed to the USB console.

If the password is lost, the USB console command `reset-password` saves a new web password without changing other settings; then enter `reboot`. To obtain the SNMP community, log in to the web and perform an explicit configuration export containing secrets. For diagnostic output there are `status` and `help`.

Next disconnect USB, connect MOD-RS232/UPS and PoE. Find the gateway address in the switch/router DHCP leases by MAC or name `ups-gateway` and open `http://GATEWAY_IP/`. If configuration with USB connected is needed, use the Ethernet port **without PoE**. The web uses a login form and HTTP session, SNMP uses v2c; place the gateway in a trusted management network. TLS and SNMPv3 are absent in this version.

## 3. UPS primary setup

Initial parameters: driver `megatec_q1`, **2400 8N1**, period 2000 ms, response timeout 1200 ms, connection loss after 10000 ms.

1. Wait for the "Communication with UPS established" state and growth of the successful poll counter.
2. Compare input/output voltage, load, input frequency and temperature with UPS indication.
3. "Protocol diagnostics" shows the last accepted Q1 string and status bits.
4. If there is no connection, check the cable and UART parameters. Do not enable control commands to find the protocol.

Q1 does not report reliable charge percentages and remaining runtime; they are shown as unavailable. Measured input frequency is not reported as output frequency. A general fault is not reported as battery replacement or selftest failure.

On the main page load is shown as a number and a vertical APC-style segment bar. The value is taken from `load_percent`, the threshold from current alarm parameters. On an unavailable/stale measurement the indicator becomes inactive: loss of connection is not shown as 0% load.

"Raw battery voltage" is shown separately. The default multiplier is 0: recalculated battery circuit voltage is not published in SNMP. If the response contains the whole circuit voltage, set 1. If the response unit and cell count are reliably known, set the corresponding multiplier. There is no automatic guessing of 24/48/72 V.

For Sinus EVO configurations specified by the owner, **with Q1 voltage on one 2-volt cell** the following multipliers are used:

| Model | Battery circuit | `battery_multiplier` |
| --- | --- | --- |
| Sinus EVO 1000 | 24 V, 2 × 12 V | 12 |
| Sinus EVO 2000 | 48 V, 4 × 12 V | 24 |
| Sinus EVO 3000 | 72 V, 6 × 12 V | 36 |

For example, 2.25 V × 36 = 81 V for model 3000. This is a conditional recalculation, not a separate measurement: nominal circuit 72 V is not equal to the current charging voltage. Do not apply these multipliers if Q1 already reports the voltage of the whole block or circuit. Charge and runtime are not calculated from one voltage alone; without driver data the line shows `Runtime remaining: —`.

**All configuration changes apply after a gateway restart.** Saved configuration and current working parameters are intentionally separated.

## 4. Observium CE and traps

Add the local device definition from `integration/observium/`: it recognizes `ESP32 UPS Gateway/...` by sysDescr and connects PowerNet-MIB. The gateway does not assign itself a specific APC model ID; sysObjectID is `0.0` until its own ID is assigned. Therefore installing the supplied definition is part of discovery setup.

Example verification from a computer with Net-SNMP installed:

```bash
snmpget -v2c -c 'YOUR_READ_COMMUNITY' 192.168.1.50 1.3.6.1.2.1.1.1.0 1.3.6.1.4.1.318.1.1.1.8.1.0
snmpwalk -v2c -c 'YOUR_READ_COMMUNITY' 192.168.1.50 1.3.6.1.4.1.318.1.1.1
```

`upsCommStatus` must be 1 on working exchange and 2 on its loss. Exact types, units and missing indicators are listed in `docs/OIDS.md`.

Control via SNMP (after setting Write Community and enabling control permissions), for example a short battery test:

```bash
snmpset -v2c -c 'YOUR_WRITE_COMMUNITY' 192.168.1.50 1.3.6.1.4.1.318.1.1.1.7.2.2.0 i 2
```

For NUT: driver `snmp-ups`, `mibs = apc`, `community` = Write Community; `upscmd -l` shows available commands. The object table is in `docs/OIDS.md`.

For events configure `snmptrapd` and a receiver in the ESP32 web interface. Traps usually arrive on UDP 162, polling on UDP 161. The package includes an example receiver and event-to-device binding via syslog. In CE you should not rely on the paid Syslog Alerting mechanism; the event log and external notifications are different functions. Detailed instructions: `integration/observium/README.md`.

The "Send test notification" button sends an explicitly marked test channel and does not control power. The `sent` counter only confirms UDP packet handover to the network stack. TRAP has no delivery acknowledgment.

## 5. Control

Regular command permission and power command permission are separate settings; both are disabled at first launch. Enable them after checking that your UPS supports the corresponding commands. Only operations declared by the driver are available.

| MegaTec Q1 operation | Command family |
| --- | --- |
| Toggle buzzer | Q |
| Short test | T |
| Test to battery discharge | TL |
| Timed test | Tnn, 1–99 minutes |
| Cancel test | CT |
| Cancel shutdown / restore output in supported state | C |
| Delayed output shutdown | Snn, delay 1–10 minutes; S.n — 12–54 s with step 6 |
| Shutdown and request output return | SnnRmmmm / S.nRmmmm, return in 3–9999 minutes |

Inform Sinus EVO capabilities and protocol, including what the UPS does not report and what cannot be configured over RS-232: `docs/SINUS_EVO.md`.

Power operations require additional confirmation on the page. `sent_unconfirmed` means "command sent, execution not confirmed". There are no retries of control commands on unknown result. Canceling shutdown is not a guaranteed cold start on any UPS.

If the PoE switch is powered from a controlled output, output shutdown can turn off the ESP32 itself. Power return must then be provided by the UPS itself or an independently powered gateway. The gateway scheduler cannot execute the next command while the gateway itself is without power.

### Schedules, time and logs

The schedules page supports up to eight rules: one-off at UTC or weekly in a given time zone. Time must be reliable, UPS connection fresh, and control and the specific operation permitted. A configured rule is saved together with the configuration and takes effect after a restart. Before saving, a separate verification screen is shown.

The scheduler does not execute missed commands retroactively. Before queuing an operation it saves a mark in NVS; on save error the command is not sent. At start and time jumps the execution window is suppressed so as not to repeat questionable operations. This is a no-resend guarantee in the described cases, not confirmation of execution by the UPS itself.

Measurement history is a RAM ring of 240 records; at a 60-second interval up to four hours, after a restart it is lost. The event log is also stored in a separate flash partition: paginated CSV export is available, see [PERSISTENT_LOGS.md](docs/PERSISTENT_LOGS.md). Recent events may remain in the buffer and be lost on sudden power-off; storage state and overflow are shown by the interface. Event clearing does not clear the SNMP trap queue. UDP syslog, like traps, does not confirm delivery.

The administrator configures the system; the operator reads data and sends permitted UPS commands; the observer only reads data. Background reading updates do not extend the session timeout without human action. All permission restrictions are enforced server-side, independently of button visibility in the browser.

## 6. Build from sources

The same `requirements.txt` installs PlatformIO and esptool. Command:

```bash
python tools/build.py
```

Run it with Python from venv. For example, on Windows:

```powershell
.\.venv\Scripts\python.exe tools\build.py
```

The script disables PlatformIO telemetry, builds the target, verifies images and updates `firmware/` together with checksums. On first build internet is required for pinned dependencies.

PlatformIO Core 6.1.18, espressif32 6.12.0, Arduino ESP32 2.0.17, ArduinoJson 6.21.5 are pinned. The web page is automatically compressed and embedded into the application; `uploadfs` is not needed. Page source — `web/index.html`.

### Release packaging

`python tools/build.py` remains a normal build and does not pass release checks. If the ZIP path is already taken (including by a symlink) or `dist/` is a symlink, `python tools/package_release.py` stops before checks, moves nothing and deletes nothing; the old archive must be removed manually. Otherwise the command creates a temporary snapshot, verifies images and performs local snapshot checks. Until at least one of them has status `PASS`, the command returns a nonzero code without a new ZIP. On success the verified ZIP is published without overwriting an existing file. Protection covers accidental conflicts and parallel packagers, but not intentional replacement of `dist/` by the same user during a run. Diagnostics without packaging: `python tools/release_gate.py`. There is no check bypass.

Project sources in `src/`, `lib/`, `web/`, `tools/`, `integration/` are checked, including C++ headers, PHP, Arduino `.ino` and JavaScript in `web/index.html`. Unknown extensions block release. The fingerprint includes all ZIP files, tests and gate configuration; old reports are not accepted. Output contains commands, versions of available tools, fingerprints and statuses. Snapshot modification during checks blocks packaging; subsequent working tree changes do not get into the archive. The archive exactly matches the verified snapshot, but the snapshot is not guaranteed to reflect a single moment of a tree being edited simultaneously. Code changes after building firmware also require rebuilding: source matching an already built image is not confirmed by this fingerprint.

Semgrep uses only local rules `tools/semgrep-release.yml`, without a cloud rule set and telemetry; any match blocks release. This is a small baseline set for unsafe C copying, Python shell calls, dynamic JavaScript execution and PHP code/shell execution calls — **not a full security check**. Trivy locally scans sources and `requirements.txt`; any vulnerability, secret, configuration error or license finding blocks release. The detected package list must exactly match pinned dependencies. Gate places its own interpreter directory first in the PATH of child processes. Any license entry is considered a finding and causes `FAIL`; absence of entries means license data is not confirmed and causes `BLOCKED`. Therefore under the current rule Trivy gate cannot obtain `PASS` while `requirements.txt` contains pinned packages, until the owner defines allowed licenses and dependency coverage for PlatformIO, which Trivy currently does not recognize. Trivy may download a vulnerability database; sources and results are not sent. Missing database, timeout, error or incomplete report blocks release.

100% of executable lines and branches by integer counters of each own file is required, CRAP strictly less than 10, no unconfirmed duplicates with similarity from 0.8, and no surviving/uncovered mutations at a nonzero number of executed mutations. Installed Crapper, Dryer and Mutator do not support own C/C++ and embedded HTML/JS; available test runners also do not provide full coverage of these files. Therefore these checks currently honestly return `BLOCKED`, and a release ZIP is not created. Partial Python checks are not evidence of firmware quality or real hardware testing.

## 7. Update preserving configuration

When transitioning from 0.1.0, schema 1 configuration is read and supplemented with schema 2 fields in memory. Passwords, communities and other old values are preserved. The read itself does not overwrite the blob; the new schema is written on explicit save. Downgrade after writing schema 2 is not supported by old firmware: first use the raw copy made before updating.

After receiving a new package version or running `tools/build.py`, perform a one-time USB transition from the old `factory` table:

```bash
python tools/flash.py --port /dev/ttyUSB0 migrate
```

For subsequent USB updates use:

```bash
python tools/flash.py --port /dev/ttyUSB0 update
```

On Windows specify `--port COM5`. `migrate` accepts only the exact previous table; `update` — only the new OTA table. An unknown table is rejected. Both commands save a raw `appcfg` copy in `backups/`, compare SHA-256 before and after, write the application to `ota_0` and only after successful write erase `otadata` so that the new application loads. `migrate` additionally writes the bootloader and then the table; `update` does not change them. Details on recovery after interruption and web update — in `docs/OTA.md`.

| Section | Address | Size | Purpose |
| --- | --- | --- | --- |
| nvs | 0x9000 | 20 KiB | ESP32 system parameters |
| otadata | 0xE000 | 8 KiB | OTA slot selection |
| ota_0 | 0x10000 | 4032 KiB | First application slot |
| ota_1 | 0x400000 | 4032 KiB | Second application slot |
| logs | 0x7F0000 | 1024 KiB | Raw ring of persistent event log |
| appcfg | 0xF00000 | 64 KiB | Gateway configuration |

`logs` is not a filesystem: it contains a persistent event log; measurement history remains in RAM. `logs` address and size are an update contract — do not move them in a new version. Ordinary `install`, `update` and `migrate` do not write or erase `logs`.

Settings preservation cannot be guaranteed with `erase_flash`, another full 16 MB firmware, appcfg transfer or physical flash failure. The standard script performs full erase only with the explicit `erase` command; the script does not perform the other listed cases. Do not use a combined image with appcfg area filling: the package intentionally contains separate images. On unknown version/NVS corruption the firmware does not erase it; web shows an error, configuration write is blocked.

Backup separately:

```bash
python tools/flash.py --port /dev/ttyUSB0 backup-config --file my-appcfg.bin
```

Restore explicitly replaces only the appcfg area:

```bash
python tools/flash.py --port /dev/ttyUSB0 restore-config --file my-appcfg.bin --confirm RESTORE_CONFIG
```

Full erase destroys both firmware slots, `otadata`, `logs`, `nvs` and `appcfg` (all settings, passwords and communities); no automatic copy is created, so if needed run `backup-config` first.

```bash
python tools/flash.py --port /dev/ttyUSB0 erase --confirm ERASE_FLASH
```

After erasing you must run `install`.

Raw copies and JSON export contain passwords/communities; keep them private. Normal settings reading via API returns secrets as empty strings; an empty string on save means "keep the previous secret".

## 8. Modularity and verification

| Directory / module | Responsibility |
| --- | --- |
| lib/UpsCore | Common state model, driver interface, MegaTec Q1, registry |
| src/ups_service.* | Single UART owner, polling, command queue |
| lib/SnmpCore | Limited BER and SNMPv2c without Arduino dependency |
| lib/GatewayCore | Configuration/validation, events, PowerNet adapter |
| src/snmp_service.* | UDP and trap queue |
| src/config_store.* | Storage in separate NVS |
| lib/GatewayCore/src/auth_core.* | Sessions, roles, CSRF, routes, HTTP parameter parsing |
| lib/GatewayCore/src/management_core.* | History, alarms, scheduler decisions, syslog format |
| src/management_service.* | NTP, NVS schedule log, separate syslog task |
| src/web_service.* | HTTP API, handlers, authorization and request body limits |
| web/index.html | Web interface |

Another UPS is added by a new `UpsDriver` and registration line. The driver sets requests, parser, frame boundaries, parity/stop bits and control capabilities. SNMP and web continue reading the shared model. Detailed procedure and model limitations — `docs/DRIVERS.md`.

Computer tests with g++, Node.js 18+ for JavaScript verification and installed dependencies:

```bash
python tools/test_host.py
python -m unittest discover -s tests -p 'test_*.py'
python tests/http_adapter/run.py
python tools/test_event_bus.py
node tests/ui_fixture_contract_check.js
```

Separate storage verification uses test NVS, random data generator and JSON implementations; it does not test the real board and its equipment:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  -Itests/platform/fakes -Isrc -Ilib/GatewayCore/src \
  tests/platform/test_config_store.cpp src/config_store.cpp \
  lib/GatewayCore/src/config_model.cpp -o /tmp/esp32-ups-gateway-test-config-store && \
  /tmp/esp32-ups-gateway-test-config-store
```

With sanitizer support:

```bash
ASAN_OPTIONS=detect_leaks=0 python tools/test_host.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python tests/http_adapter/run.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python tools/test_event_bus.py --sanitize
```

LeakSanitizer disable relates to an environment with ptrace where verification was performed; ASan/UBSan remain enabled. On your own computer you can check for leaks without this variable. Historical report of the original build and brief current state are in `docs/BUILD_REPORT.md`.

After installation be sure to verify real connection, matching readings, trap reception, Observium sensors appearing and settings preservation after one normal update. These are hardware checks; a successful build does not replace them.

## 9. New interface preview

For local preview without ESP32 open `tests/ui_fixture.html` in your browser. This is a separate test shell using the same interface code from `web/index.html`. The top bar explicitly marks synthetic data; buttons allow checking normal/high load, connection loss, HTTP error and session end. `Fill Login` fills only test credentials. This file is not embedded in firmware and does not send commands to the device.

Recreating after UI change:

```bash
python tools/make_ui_fixture.py
```

The volume of actual verification performed, including presence or absence of browser rendering, is recorded in `docs/BUILD_REPORT.md`. The presence of the test HTML itself does not mean the interface is verified with screenshots.

## License

The project is licensed under the GNU General Public License v2.0 or (at your option) any later version (SPDX: GPL-2.0-or-later), see LICENSE; third-party components keep their own licenses, see THIRD_PARTY.md.
