> Historical specification version 0.1.0. 0.2.0 changes are described in
> `APC_V02_SPEC.md` and `APC_PORT.md`; current OTA layout — in `OTA.md`.

# ESP32 UPS Gateway — specification

User requirements: ESP32-POE WROOM with 16 MB flash, MOD-RS232 on UEXT;
SNMP APC PowerNet subset for Observium CE; SNMP traps; web dashboard and
configuration/control; a single application slot; persistent configuration
across normal serial firmware updates; modular UPS drivers.

Inform Sinus EVO speaks MegaTec Q1; the selectable driver is
`megatec_q1`. Never manufacture charge/runtime or claim hardware validation
that was not actually performed. Destructive controls require an explicit
configuration opt-in and per-command confirmation. No automated output cycling.

Implementation: PlatformIO espressif32 6.12.0, Arduino ESP32 2.0.17,
C++17, ArduinoJson 6.21.5, built-in ETH, esp_http_server, NVS. One factory app at
0x10000; default system NVS at 0x9000; separate appcfg NVS at 0xF00000,
size 0x10000. Never automatically erase appcfg on an error. UART TX4/RX36;
LAN8720 addr0 MDC23 MDIO18 power12 clock GPIO17_OUT. USB-only firmware
updates are intentional with a single application slot.

Core modules must not depend on Arduino: UpsCore (driver contract, status,
MegaTec parser, event detection) and SnmpCore (bounded BER, GET/GETNEXT/
GETBULK, read-only SET rejection, v2 trap encoding). Application modules:
config store/validation, UART transaction scheduler, PowerNet adapter,
UDP transport, authenticated HTTP service and embedded web resources.

Measurements carry validity. Q1 does not supply battery percentage,
runtime, measured output frequency, battery-specific temperature, overload or
replacement status. Expose only supported values. Q1 raw battery voltage
is displayed; PowerNet pack voltage requires an explicit multiplier.

SNMPv2c traps are mandatory. INFORM is not claimed unless fully implemented.
Every event is generated from a transition, not every poll. Link loss and
recovery are supported. Acknowledgement of a web command is not hardware
confirmation. SNMP controls are read-only in v1 of this project.

Configuration includes Ethernet DHCP/static, hostname, UPS driver/baud/
polling/timeouts/battery scaling, SNMP RO community/optional allowed IPv4,
trap destinations/community, web credentials, control opt-in. Secrets are
redacted from ordinary config reads; authenticated explicit export may
contain them and is labelled. Writes validate before persisting, use one
versioned NVS record and keep the old config if a write fails. Fresh source
builds use the factory administrator pair `ups` / `ups`; replace the weak
password immediately. Other administrator passwords require 12–64 bytes.
SNMP community remains randomly generated. First-boot credentials are printed
only to the local USB serial console. No default public write access. The
bundled firmware image has been rebuilt with this factory-login change; it has
not been tested on physical hardware.

Deliver full source, tested ESP32 build if environment permits, raw split
flash images, SHA256 checksums, English build/install/operation docs,
driver-extension guide and Observium CE trap receiver example. Never ship
a merged image that overwrites appcfg with padding.


PowerNet upsAdvBatteryTemperature is documented as internal UPS temperature despite its historical name; map the Q1 UPS-temperature measurement to that object with its defined nonnegative scale, never label it as a measured battery sensor in the UI.
