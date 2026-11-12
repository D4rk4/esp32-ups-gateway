# UPS Gateway 0.2 — UI / API coverage

Production source: `web/index.html`. It is embedded by `tools/embed_web.py` as deterministic gzip into the existing application image. It contains no synthetic measurements, test credentials or fixture server. No external fonts, scripts, styles or CDN resources are required.

## Shell and session

The 750 px desktop shell, 140 px left navigation, 10–11 px Verdana typography, blue/pale-blue palette, compact tables and beveled controls follow the extracted classic APC AOS/RPDU reference. Decorative GIF/JPEG resources are embedded as data URIs. Branding is **UPS Gateway**, with explicit independent-product attribution. A narrow viewport scrolls horizontally; it is not redesigned as a modern card dashboard.

| UI action / page | Real route | Roles / behavior |
|---|---|---|
| Initial session check | `GET /api/session` | Public unauthenticated response opens the login shell. Authenticated response supplies active display preferences, driver capabilities, permissions and timeout. |
| Log On | `POST /api/login {username,password}` | Cookie session; no Basic authentication and no credential storage in local/session storage. |
| Log Off | `POST /api/logout {}` | CSRF-protected; login form shown after success. |
| Background refresh | `GET /api/status`, `/api/alarms`, `/api/time` | Authenticated roles. Five-second refresh; background GETs do not refresh idle expiry. `X-Session-Activity: 1` is sent only after real input or a user-initiated read. |
| Expired session | Any protected route returns 401 | Abort outstanding requests, clear secret inputs/config state, hide application and show login explanation. |
| Session role | `permissions` in session response | Read-only users see live data, logs, schedules and capabilities. Operators additionally review/submit permitted UPS commands. Configuration pages are admin-only. Server authorization remains authoritative. |

Every mutation other than login sends the per-session `X-CSRF-Token`. HTTP and JSON errors are shown in the page banner or confirmation dialog. Fetches have a bounded timeout. No failed submission is displayed as a successful save.

## Functional pages

| Page / visible workflow | API | Details |
|---|---|---|
| Home → Overview | `GET /api/status`, `/api/alarms`, `/api/events?min_severity=1&contains=&limit=5` | Live status, gateway identity, measured load and electrical summary, recent events. Missing metrics remain unavailable. |
| APC-style load indication | `status.ups.load_percent`, `status.thresholds.alarm_load_high`, `alarm_delay_s` | Narrow vertical gauge with 20 beveled segments and adjacent numeric percent. Display scale 0–100%, with actual numeric value retained above 100%. Green below 80% of active high threshold, amber from 80%, red at/above threshold. Color is immediate indication; delayed alarm state comes from `/api/alarms`. Missing/stale data clear the gauge and show an em dash, not 0%. Freshness requires the returned sample age plus time since response receipt to remain below active `ups.stale_ms`, with a maximum client cache age of 15 s. A one-shot local expiry repaint invalidates the gauge and command gates even while a subsequent HTTP refresh is still in flight; the HTTP polling interval is unchanged. Invalid sample ages/bounds fail closed; only an absent legacy fixture bound uses the conservative 700 ms minimum. Active thresholds are read-only for every role; unsaved/saved pending thresholds do not change the running gauge. |
| Home → Alarm Status | `GET /api/alarms`; admin `POST /api/config` threshold patch | Physical flags, link and configured threshold alarms remain distinct. Integer load/temperature/voltage/delay fields match backend bounds; temperature threshold remains Celsius. |
| Device → Status | `GET /api/status` | Measured electrical values, driver identity, firmware, gateway network and protocol diagnostics. Unsupported battery percent/runtime/output frequency are not invented. |
| Device → Control | `GET /api/status`, `/api/capabilities`; `POST /api/command` | Driver bitmask, role, active opt-ins, current link and queue state gate each operation. Step 1 selects a command. Step 2 shows the command, whole-UPS target, shutdown/restore delays and delivery limitation. Power operations require typed `POWER`; others use `EXECUTE`. Status is refreshed again before enqueue. No outlet groups are fabricated. |
| Device → Serial / UPS | `GET /api/config`; `POST /api/config` serial/control patch | Driver, all supported baud rates including 115200, poll/response/stale bounds, battery multiplier and control opt-ins. Applies after reboot. |
| Device → Schedules | `GET /api/schedules`; `GET /api/config`; `POST /api/config {schedules:[…]}` | Active runtime table includes armed/reason/last result. Separate saved editor supports add/edit/remove, at most eight unique IDs, one-time UTC and weekly device-time rules. Local list changes require Apply. Review shows operations and delays. |
| Schedule power consent | `POST /api/config` with transient top-level `confirm:"POWER"` | Fresh typed consent for new/changed enabled power rules, enabling retained power rules through control opt-ins, driver changes, or weekly timezone changes. Import has the same checks. An imported `confirm` property is discarded; no token is synthesized from the file. Confirmation never persists in config. |
| Logs → Events | `GET /api/events?min_severity=…&contains=…&limit=48` | Server-side severity/text filtering. UTC timestamps are explicitly marked UTC; unavailable UTC falls back to device uptime. |
| Event export / clear | `GET /api/events.csv` with the same filters; `POST /api/logs/clear {target:"events",confirm:"CLEAR_LOG"}` | CSV download; admin confirmation dialog for clear. Explains independent SNMP delivery queue. |
| Logs → Data | `GET /api/history?limit=240` | Selectable SVG graph and recent sample table. Samples without valid connected metrics create gaps; no zero substitution. Stored Celsius is converted using active display preference. |
| Data export / clear / interval | `GET /api/history.csv`; `POST /api/logs/clear {target:"history",confirm:"CLEAR_LOG"}`; `POST /api/config {history_interval_s:…}` | RAM volatility/capacity stated. Admin config interval 10–3600 s applies after reboot. |
| Administration → Security | `GET /api/config`; `POST /api/config` users/session patch | Administrator, enabled/disabled operator and read-only accounts; distinct enabled usernames; session 1–240 min. Blank secrets retain stored values; UTF-8 byte lengths are checked. |
| Administration → Network / IPv4 | `GET /api/config`; `POST /api/config` IPv4 patch | DHCP/static IP, netmask, gateway and DNS; byte/address/subnet validation. Active IP shown separately. |
| Administration → Network / SNMP | `GET /api/config`; `POST /api/config` SNMP patch | Actual implemented service is **SNMPv2c**. Community remains blank/redacted on normal reads; NMS IPv4 filter. SNMPv1 is not claimed. |
| Administration → Notification / traps | `GET /api/config`, `/api/status`; `POST /api/config {traps:[…]}`; `POST /api/trap-test {}` | Two receivers, UDP port, blank-preserved communities. Test explicitly uses active configuration and reports queuing without claiming recipient delivery. |
| Administration → Notification / syslog | `GET /api/notifications`; `POST /api/config` syslog patch; `POST /api/syslog-test {}` | Enable/host/port/minimum severity; active runtime stats and test queue status. UDP delivery is not confirmed. |
| General → Identification | `GET /api/config`; `POST /api/config {hostname,location,contact}` | ASCII hostname; location/contact max 96 UTF-8 bytes. |
| General → Date & Time | `GET /api/time`; `GET /api/config`; `POST /api/config {ntp_enabled,ntp_server,timezone}` | Active device-local time/POSIX timezone shown separately from saved form. UI does not reinterpret device time in the browser's timezone. |
| General → manual clock | `POST /api/time {utc:…}` | Explicit UTC input and confirmation; disabled while active NTP is enabled. Picker limits and validation use `time.min_utc` / `time.max_utc` from the actual target clock. The pinned target maximum is 2038-01-19 03:14:07 UTC; no unconditional 2099 support is claimed. |
| General → Preferences | `POST /api/config {date_format,temperature_unit,default_page}` | Three date formats; C/F; Home/Status/Events/Data default page. Active session preferences remain until reboot. |
| General → Quick Links | `POST /api/config {quick_links:[…]}` | Up to three title/http(s)-URL pairs; 32/192 UTF-8 byte bounds. Active links open with `noopener noreferrer`. |
| General → Export | `GET /api/config/export` | Admin-only explicit warning; downloaded JSON contains actual saved secrets. No secrets are printed in the UI. |
| General → Import | `POST /api/config` | Schema 1/2 JSON up to 12288 bytes, review before submission, server atomic validation, reboot required. Scheduled power changes require fresh typed consent. |
| General → Reset | `POST /api/reset-config {confirm:"RESET_CONFIG",keep_network:boolean}` | Custom dialog accurately states access credentials/communities are preserved, optionally IPv4. Not labeled a complete flash erase. |
| General → Reboot | `POST /api/reboot {confirm:"REBOOT"}` | Custom dialog explains temporary connection/session loss, pending config and potential changed IP. No UPS power command implied. |
| About / Capabilities | `GET /api/capabilities`, `/api/about` | Real server-supported function matrix, hardware/storage/protocol diagnostics, attribution and unsupported service descriptions. |

## Non-implemented original APC services

SNMPv3, HTTPS, SMTP, RADIUS, SSH, Telnet, FTP and WAP appear only in the API-driven informational capabilities table. There are no dummy settings or simulated live services for them. UPS outlet groups are absent unless a driver/API actually supplies them; the current UI explicitly describes whole-UPS commands.

## Saved versus active configuration

Forms read the saved redacted config once after login. Regular status polling never repopulates inputs or overwrites drafts. Each Apply submits only its section's keys; trap/link/schedule array edits replace only that array. Secret blanks are preserved by the server. Successful writes set a persistent reboot-required banner. Active telemetry, command gates, time, quick links, units and thresholds continue to use the running API/session configuration.

## Fixture and verification boundary

`tools/make_ui_fixture.py` injects `tests/ui_fixture_api.js` before the actual production script and creates `tests/ui_fixture.html`. The fixture is visibly marked **SYNTHETIC API DATA / NO PHYSICAL UPS / NO NETWORK**, blocks native network fetches with its CSP and fetch handler, and is not embedded in firmware. It supports three-role login, normal/high/lost UPS data, HTTP error, expiry, configuration saves with pending/active separation, commands, schedules, logs/CSV, notifications and reboot/reset.

`node tests/ui_fixture_contract_check.js` executes the actual production script in a non-browser VM with a minimal document harness and checks critical rendering/validation/API boundaries. `python tests/web_ui_check.py` is a compatibility entry point for that check; it no longer launches obsolete v0.1 Playwright tests.

**Not performed:** browser layout rendering, real-browser interaction validation and screenshot capture. The available managed browser explicitly rejected the local fixture navigation by policy. No alternate browser or workaround was used, and no screenshots were fabricated. The fixture can be opened locally by the user for the remaining visual checks. These tests do not validate ESP32 hardware, a physical UPS, real Ethernet/session cookies or delivery of notifications.
