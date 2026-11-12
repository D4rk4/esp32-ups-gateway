# APC-style web management — version 0.2.0

## User goal and implementation choice

The user supplied `apc_hw02_aos394_rpdu_393.exe` and asked to extract its web
logic, appearance and layout. They then explicitly requested extending the
ESP32 API for missing interface functions, a new firmware/source ZIP, browser
validation and screenshots delivered separately from that ZIP.

Build on working 0.1.0 and its modular driver architecture. Port the classic
APC navigation and management workflows to the UPS gateway. Keep the clear
750 px desktop composition, 140 px sidebar, blue/grey palette, compact tables
and Verdana typography; permit horizontal scrolling on small viewports.
Brand the live firmware as UPS Gateway.

The user subsequently explicitly required APC-like load indication. Use its
narrow vertical beveled gauge with 20 segments and measured UPS percentage,
active threshold coloring, and an unmistakable missing/stale state.

The original product is a PDU. UPS output groups are represented by driver
capabilities, never fabricated. Do not expose functioning-looking switches
for absent physical outlets. The port matrix must explicitly distinguish
implemented gateway workflows from original platform services not provided
by this firmware (SNMPv3, HTTPS, SMTP, RADIUS, SSH/Telnet, FTP, WAP).
These services have no live configuration controls in this release; a
capabilities/help page states their status. This is an implementation scope
decision, not a claim that the user explicitly selected those exclusions.

Every visible input, button and live page must use a real API route and
persist or perform the stated action. No "coming soon" controls or local-only
configuration state in the production UI. Demo data is restricted to test
fixtures, explicitly labelled in screenshots, and excluded from firmware.

## Preserved contracts

- ESP32-POE WROOM 16MB, MOD-RS232 TX4/RX36, existing LAN8720 configuration.
- One factory app 0x10000/0x3F0000; appcfg 0xF00000/0x10000. No flash-layout
  changes or automatic NVS erasure. USB update retains the backup/hash check.
- Inform Sinus EVO uses MegaTec Q1 via the `megatec_q1` driver.
- Missing UPS values remain null; stale samples never become live values.
- Commands require role, capabilities, fresh status and control opt-ins.
  Power commands additionally require POWER confirmation. Sending is not ACK.
- Configuration changes apply after reboot, including users and schedules.
  Saved and active config must remain visually distinct.
- All current API keys and command names remain, except web authentication
  now uses cookie login/session flow. Basic auth is removed in this release.
- Network services run without blocking the UART polling owner.

## New functional pages

Home: Overview, Alarm Status. Device Manager: Status, Control, Serial/UPS
configuration, Schedules. Logs: Events, Data. Administration: Security
(users/session timeout), Network (IPv4/SNMP), Notification (traps/syslog),
General (identity/time/units/quick links/config backup+import/reset/reboot),
About/Capabilities. Two-step command review must show operation and delays.

## Config schema 2: exact additional fields

Existing schema-1 fields retain names. New fields are optional when migrating
schema 1; schema 2 exports include all fields. Reject unknown future schemas.
Migration occurs in RAM on load, writing only on an explicit save. Preserve
old bytes on corrupt/unsupported data or failed writes.

```json
{
  "schema": 2,
  "operator_enabled": false,
  "operator_user": "operator",
  "operator_password": "",
  "readonly_enabled": false,
  "readonly_user": "viewer",
  "readonly_password": "",
  "session_timeout_min": 15,
  "ntp_enabled": true,
  "ntp_server": "pool.ntp.org",
  "timezone": "UTC0",
  "date_format": "YYYY-MM-DD",
  "temperature_unit": "C",
  "history_interval_s": 60,
  "default_page": "home",
  "quick_links": [{"title":"","url":""},{"title":"","url":""},{"title":"","url":""}],
  "alarm_load_high": 90,
  "alarm_temperature_high": 50,
  "alarm_input_low": 180,
  "alarm_input_high": 260,
  "alarm_delay_s": 5,
  "syslog_enabled": false,
  "syslog_host": "",
  "syslog_port": 514,
  "syslog_min_severity": 1,
  "schedules": []
}
```

New disabled-account passwords may be empty; enabling requires 12–64 bytes.
Usernames max32 bytes, distinct across enabled roles. Secret blank edits retain
existing values; export includes actual secrets. Normal GET adds *_password_set
flags. Admin credentials remain existing web_user/web_password.

Bounds: session1..240min; NTP/syslog host1..63 ASCII host or IPv4; timezone
1..96 printable ASCII POSIX TZ; date_format one of YYYY-MM-DD/DD.MM.YYYY/MM/DD/YYYY;
unit C/F; history10..3600s; alarm load1..200%, temperature1..150C,
input low1..300V/high2..400V and low<high, delay0..300s. Quick links max3,
title32bytes/url192bytes, empty pair or http(s) URL only. default_page one of
home/status/events/data. Syslog port1..65535 and min_severity1..3.

Schedules: at most8 records, unique id1..65535, enabled bool, name<=48bytes,
command from existing driver commands, delay_minutes/restore_minutes as in
the driver, mode once/weekly; once uses at_utc; weekly uses days_mask (bit0
Sunday through bit6 Saturday) and minute_of_day0..1439 in configured timezone.
Sample:
{"id":1,"enabled":false,"name":"Weekly test","command":"test_short",
 "delay_minutes":1,"restore_minutes":3,"mode":"weekly","at_utc":0,
 "days_mask":2,"minute_of_day":540}

Scheduling default disabled by an empty list. Persist an execution claim
before enqueue, never replay uncertain/old actions after a restart/time jump;
if claiming fails, do not send. Never catch up overdue commands. Revalidate
permissions/driver/status on due execution. Journal last result and include
armed/reason information in API. Test scheduling with fake time and recording
transport, not by sending power commands. One-minute due window maximum.

## Runtime management contract

Create `src/management_service.hpp/.cpp`, `gateway::ManagementService`:

```cpp
ManagementService(const Config&, UpsService&, EventBus&);
bool begin();
void tick();
std::string alarmsJson() const;
std::string historyJson(size_t limit = 240) const;
std::string historyCsv() const;
std::string timeJson() const;
std::string schedulesJson() const;
std::string notificationsJson() const;
bool setTime(uint64_t utc_seconds, std::string& error);
void clearHistory();
bool testSyslog(std::string& error);
```

Implementation may add private helpers but these signatures are the shared
HTTP integration boundary. Pure filtering/history/alarm/scheduler decisions
belong in `lib/GatewayCore/src/management_core.*` and host tests.

History: RAM ring240 samples, each seq,uptime_s,utc(nullable),connected and
valid/null input_voltage/output_voltage/load_percent/input_frequency/
ups_temperature/battery_voltage/battery_charge/runtime_seconds. Sample at
history_interval_s, including missing/stale as null. JSON:
{"samples":[],"capacity":240,"interval_s":60,"volatile":true}.
CSV labels describe UTC versus uptime, escape formula-like text if any.

Alarms JSON: {"alarms":[{"id":"link","severity":3,"message":"...",
"since_uptime_s":123}],"critical":1,"warning":0}. Include actual link,
battery, low-battery, fault and shutdown flags plus configurable thresholds.
Threshold transitions are logged; do not relabel a configured threshold as
a physical UPS overload or emit an unrelated APC trap number.

Time JSON also returns min_utc/max_utc from the actual target time_t range.
Example: {"valid":true,"utc":1794000000,"local":"...",
"timezone":"UTC0","ntp_enabled":true,"ntp_server":"pool.ntp.org"}.
Manual time rejects when NTP is enabled. NTP uses the framework SNTP service.

Schedules JSON: {"rules":[],"clock_valid":true}; each rule includes stored
config and runtime armed/reason,last_claim,last_state,last_message.
Syslog is bounded UDP RFC3164, uses configured host/port and existing event
severity1info/2warning/3critical. No confirmed-delivery claim. Events are read
independently from log snapshots; never consume SNMP's trap queue.

Extend EventBus with clearLog() and UTC timestamp in Event if convenient.
Clear the visible event journal, not pending trap delivery. Preserve current
48-entry capacity unless a measured reason requires change; expose capacity.

ConfigStore adds `resetDefaults(bool keep_network, std::string& error)`, which
resets functional settings but preserves all access credentials and SNMP
communities, with optional preservation of IPv4 settings. UI must state that
access is preserved; this is not labelled a full destructive flash erase.

## HTTP contract

Use existing esp_http_server with bounded request length (max12288 for config;
small endpoints still <=2048), bounded JSON allocations and no blocking UDP
or UART work in handler. Decode/query-strip URI before dispatch.

Authentication: public GET / returns login shell. POST /api/login
{username,password} creates random RAM session; HttpOnly SameSite=Strict cookie,
random per-session CSRF. GET /api/session returns authenticated,username,role,
csrf,boot_id,firmware,drivers,permissions and active display preferences.
POST /api/logout destroys session. Expiry honors idle timeout and rejects
expired CSRF. Bounded session table and bounded failed-login throttle. Never
store passwords in browser localStorage/sessionStorage. Basic API requests,
if retained, require the existing CSRF workflow for mutations.

Roles: viewer reads state/alarms/events/history/time/capabilities; operator
also submits permitted UPS commands; admin configures/exports secrets,
tests notifications, clears logs, sets time, resets/reboots. Config getters
and users only admin. Enforce this on server, not only disabled controls.

Routes added to existing status/config/events/command/trap-test/reboot:

- GET /api/capabilities — explicit supported features and unsupported services.
- GET /api/alarms — ManagementService.alarmsJson().
- GET /api/history?limit=240 and /api/history.csv.
- GET /api/events?min_severity=1&contains=&limit=48, /api/events.csv.
- POST /api/logs/clear {target:events|history,confirm:CLEAR_LOG}.
- GET /api/time, POST /api/time {utc:...}.
- GET /api/schedules (configuration through /api/config schedules array).
- GET /api/notifications, POST /api/syslog-test {}.
- POST /api/reset-config {confirm:RESET_CONFIG,keep_network:true}.
- GET /api/about — hardware/software/storage/protocol diagnostics, no secrets.

GET /api/status includes ups.age_ms and active ups.stale_ms. The UI expires
its cached measurements at the earlier of the remaining sample freshness
window and 15 seconds, repainting the gauge and command gates at that deadline
even if another HTTP request is still pending.

For config save preserve server validation/rollback and reboot_required.
Tests must cover viewer/operator privilege escalation, invalid/expired CSRF,
schema1 migration preserving secrets, invalid config atomicity, stale metrics,
scheduler no catch-up/replay, CSV, and browser form/API roundtrips.

## Deliverables and verification

Compile actual firmware0.2.0, retain single-slot flash tools and source in ZIP.
Include extracted reference source and provenance under reference/apc-webui/,
separately attributed. Original firmware executables are not needed in ZIP.
Browser screenshots must exercise the new production HTML with clearly marked
synthetic API fixtures if hardware is unavailable, including overview,
control review, data/events, administration, and loss-of-link/error state.
Deliver requested screenshot files separately from firmware ZIP. Record
exact completed tests and distinguish browser fixture from physical hardware.
