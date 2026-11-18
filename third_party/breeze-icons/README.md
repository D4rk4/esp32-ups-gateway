# Vendored KDE Breeze icons

Upstream: https://github.com/KDE/breeze-icons
Commit: `8cb887ed16c13fe5fb9b6609f04e8157a083e046` (2016-07-03, "update emblem icons to use stylesheet color")
License: GNU Lesser General Public License, version 3 or (at your option) any later version,
"Copyright (C) 2014 Uri Herrera <uri_herrera@nitrux.in> and others". The notice is
`COPYING-ICONS`, copied verbatim from that commit; `COPYING.LESSER` is the unmodified LGPL
version 3 text from https://www.gnu.org/licenses/lgpl-3.0.txt and `COPYING.GPL-3` is the
unmodified GPL version 3 text from https://www.gnu.org/licenses/gpl-3.0.txt (LGPL version 3
is GPL version 3 plus extra permissions, so it refers to it).

The four SVG files are byte-identical to the upstream files at that commit.

| Web UI status | File here | Upstream path |
| --- | --- | --- |
| good | `emblem-checked.svg` | `icons/emblems/8/emblem-checked.svg` |
| warning | `emblem-warning.svg` | `icons/emblems/8/emblem-warning.svg` |
| critical | `emblem-error.svg` | `icons/emblems/8/emblem-error.svg` |
| info (and fallback for unknown names) | `emblem-information.svg` | `icons/emblems/8/emblem-information.svg` |

Do not edit these files. `python tools/embed_icons.py` turns them into the sprite and
`icon()` function inside `web/index.html` (the mapping lives in `ROLES` in that tool).
`tests/test_embed_icons.py` fails when `web/index.html` no longer matches them.
