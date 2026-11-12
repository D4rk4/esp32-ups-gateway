# Dependencies and protocol references

The project-specific code is supplied under LICENSE. The included firmware
also links the separately licensed Arduino-ESP32 / ESP-IDF / toolchain runtime
components. Their upstream licenses apply to those components; this project
does not relicense them. The build downloads their source/header packages at
the pinned versions. Keep upstream notices when redistributing modified builds.

| Component | Version / source |
| --- | --- |
| PlatformIO Core | 6.1.18; https://github.com/platformio/platformio-core |
| Espressif32 platform | 6.12.0; https://github.com/platformio/platform-espressif32/tree/v6.12.0 |
| Arduino ESP32 | 2.0.17, ESP-IDF 4.4.x base; https://github.com/espressif/arduino-esp32/tree/2.0.17 |
| ArduinoJson | 6.21.5, MIT; https://github.com/bblanchon/ArduinoJson/tree/v6.21.5 |
| esptool | 4.9.0; https://github.com/espressif/esptool/tree/v4.9.0 |
| SNMP operations | RFC 3416; https://www.rfc-editor.org/rfc/rfc3416.html |
| MegaTec protocol | https://networkupstools.org/protocols/megatec.html |
| NUT MegaTec implementation reference | https://github.com/networkupstools/nut/blob/master/drivers/blazer.c |
| APC-authored PowerNet MIB reference | https://networkupstools.org/protocols/snmp/APC-Powernet.pdf |
| APC-authored PowerNet-MIB definitions mirror | https://raw.githubusercontent.com/librenms/librenms/master/mibs/apc/PowerNet-MIB |

The user-supplied `ups_snmp_firmware_2026-11-10.zip` was used as a reference
for KPM220 features/protocol organization. Proprietary KPM220/DL801 firmware,
web resources, certificates and executable files are not included in this
project. The C++ implementation is newly written. Version 0.2.0 adapts the
classic APC management layout and includes selected APC image assets as
described below; it is not an APC Network Management Card firmware.

## APC web reference and assets

The user supplied `apc_hw02_aos394_rpdu_393.exe` (SHA-256
`0a2f4aebfbbcc698cbdfe0a1d30e9ff24483632b2f460d6da7ce06c7a71398be`).
Static resources from AOS 3.9.4 / RPDU 3.9.3 were extracted without running
the updater or original firmware. `reference/apc-webui/` contains the
extracted templates, CSS, images, provenance and bounded extraction tools.
The original APC executable images are not included in this release ZIP.

APC / Schneider Electric retains its rights in original extracted resources,
images, text and marks. The project MIT license does not apply to those
materials or to original image bytes embedded in the live UI. Reconstructed
templates retain explicit placeholders where the original server supplies
runtime data. Native APC server-side handlers have not been recovered or
ported as executable code.

Official source package page:
https://www.se.com/us/en/download/document/APC_RPDU_393_EN/

