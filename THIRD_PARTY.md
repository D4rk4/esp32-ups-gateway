# Dependencies and protocol references

The project-specific code is licensed under the GNU General Public License
v2.0 or (at your option) any later version (SPDX: GPL-2.0-or-later), see
LICENSE. The included firmware also links the separately licensed
Arduino-ESP32 / ESP-IDF / toolchain runtime components. Their upstream licenses apply to those components; this project
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

## Web UI status icons

The four status icons of the web UI (good, warning, critical, info) come from
the KDE Breeze icon theme, https://github.com/KDE/breeze-icons, commit
`8cb887ed16c13fe5fb9b6609f04e8157a083e046` (2016-07-03). The source files are
vendored byte for byte:

| Status | Source file |
| --- | --- |
| good | `icons/emblems/8/emblem-checked.svg` |
| warning | `icons/emblems/8/emblem-warning.svg` |
| critical | `icons/emblems/8/emblem-error.svg` |
| info (and fallback) | `icons/emblems/8/emblem-information.svg` |

Copyright (C) 2014 Uri Herrera and others. License: GNU Lesser General Public
License, version 3 or (at your option) any later version (SPDX:
LGPL-3.0-or-later), as stated in the upstream `COPYING-ICONS` of that commit.
`COPYING-ICONS` is kept verbatim in `third_party/breeze-icons/` together with
the unmodified LGPL version 3 text (`COPYING.LESSER`, from
https://www.gnu.org/licenses/lgpl-3.0.txt), the unmodified GPL version 3 text
(`COPYING.GPL-3`, from https://www.gnu.org/licenses/gpl-3.0.txt), the four SVG
files and a README with their provenance. A release ZIP built by
`tools/package_release.py` contains the whole directory. The inline
markup in `web/index.html` is generated from these files by
`tools/embed_icons.py`, which writes each icon's theme colour into the markup
instead of keeping the upstream stylesheet.

The project's own source is GPL-2.0-or-later; the embedded Breeze icons are
LGPL-3.0-or-later, which cannot be combined under GNU GPL version 2 alone, so
firmware images and release packages are distributed under GPL-3.0-or-later
through the "or later" option of the project licence; the licence texts are
`third_party/breeze-icons/COPYING-ICONS`,
`third_party/breeze-icons/COPYING.LESSER` and
`third_party/breeze-icons/COPYING.GPL-3`.
