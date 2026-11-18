# Firmware Update and Recovery

## One-time USB transition to OTA

To update an old installation with a single `factory` partition, first build a
new version via `tools/build.py` or get a set of new separate images. Old
binaries with the old partition table do not fit. Connect USB and run in an
activated venv (Windows: instead of `/dev/ttyUSB0`, specify, for example,
`COM5`):

```bash
python tools/flash.py --port /dev/ttyUSB0 migrate
```

`migrate` accepts only the exact old table `nvs`, `factory`, `appcfg`. A new
`firmware.bin` written to the old layout without `migrate` will start, but it
will show the web update as unavailable: there is no second OTA slot yet. It
does not change an unknown or damaged table. If OTA is already on the device,
use `update`, not `migrate`. Before writing, a separate raw copy of `appcfg` is
saved to `backups/`; after writing, it is read and SHA-256 compared. The copy
contains passwords and community and must be kept private.

The script first writes the bootloader and the application to `ota_0`, then
erases only `otadata` (`0xE000–0xFFFF`), and writes the partition table as a
separate last operation. With the old table, `0xE000–0xFFFF` was not used by
any partition, so its erasure does not delete data from the previous
installation. Erasing `otadata` selects `ota_0` at the next boot. According to
the stock bootloader code, at the first boot with an empty `otadata` and no
`factory`, the bootloader itself writes the correct `ota_0` selection record.
After `update` (not `migrate`) the first boot was observed to run from `ota_0`;
the serial console printed `Running partition: ota_0; OTA image state: 2`.
Rollback after the first web update is still not verified on physical
hardware. Writing the new table last allows repeating
`migrate` if the connection was interrupted after writing the application but
before completing the erasure of `otadata`: the old table is still in place.
Do not disconnect or restart power until the script reports successful
completion. If after an interruption the old table remains, repeat `migrate`;
if the new table is already written, repeat `update`. If the writing of the
table sector itself was interrupted and the table is not recognized, restore
the device via `install`; it writes the bootloader, table, and application
without checking the old table. In each case, reconnect USB, check the messages,
and do not erase the entire flash. If the `appcfg` comparison fails, save the
backup copy and find the cause before restoring or rebooting.

## Subsequent updates

Over USB for the new OTA table:

```bash
python tools/flash.py --port /dev/ttyUSB0 update
```

`update` writes only `firmware.bin` at address `0x10000`, and after a
successful verified write erases `otadata` (`0xE000/0x2000`). This guarantees
selection of the just-written `ota_0`, even if a web update previously selected
`ota_1`. The bootloader and table are not rewritten. `ota_0` is overwritten
first, even if it was selected before the update; `ota_1` is not changed. Until
successful completion, the script does not initiate a restart. Do not disconnect
or restart power until the script reports successful completion: the application
write and the erasure of two `otadata` sectors are not atomic. On interruption,
depending on the stage and slot selection, the new or old firmware may boot,
or recovery via USB may be required. Repeat `update` until successful
completion: it writes the application again and selects `ota_0`. Backup and
`appcfg` integrity check are performed the same way as for `migrate`.

In the web interface, update is available only to a logged-in administrator;
the mutating request is protected by CSRF. Upload only a whole application
image `firmware.bin` with type `application/octet-stream`, not ZIP, not a
bootloader, and not a partition table. After successful upload, the gateway
**restarts itself**; the API returns `{"ok":true,"rebooting":true}`. There is no
separate restart step. During restart, communication with the UPS and the web
interface is temporarily interrupted.

Version 0.2.0 cannot take a web upload: test uploads restarted the gateway
before a new slot was selected. The likely cause is a stack overflow: its upload
handler keeps a 4 KB buffer on the 8 KB HTTP task stack, and the computed call
depth on that path exceeds the stack; no crash log was captured to confirm it.
The step from 0.2.0 to 0.2.1 therefore needs one USB
`update` (or `migrate`) with the PoE Ethernet cable disconnected; web update
was verified from 0.2.1 onward: a whole `firmware.bin` uploaded over
`POST /api/firmware` as a logged-in administrator with the CSRF token,
`Content-Type: application/octet-stream` and `X-Firmware-Confirm: UPDATE`
answered `{"ok":true,"rebooting":true}` within about 6 s, rebooted to the other
slot (`ota_1`), and came back with a new boot id; it stayed on `ota_1` after
the 120 s verification window, configuration and the event log were preserved,
and the UPS reconnected with no poll errors. Not verified: automatic rollback
of an image that fails verification, an update between two different firmware
versions (the same image was uploaded), or an upload from a real browser.
If a gateway restarts unexpectedly, the next `startup` event in the event log
names the reset reason (for example `task watchdog` or `panic`).

HTTP is not encrypted: passwords, session, and the image may be intercepted or
modified over the network. Perform login and update only in a trusted local
network or via a protected VPN; do not expose the interface directly to the
internet.

## Rollback and partitions

Two slots allow installing a new application into an inactive slot without
overwriting the current one. On a failed web load before selecting a new slot,
the current version remains active. After starting, the new version in the
verification state gets up to 120 seconds for configuration and service
readiness, the presence of an Ethernet connection and an IP address. On
success it is marked valid; on timeout, the SDK rollback mechanism is
invoked. If the confirmed state of the current firmware is absent or not
supported, web loading is blocked. Physical rollback on hardware has not yet
been verified. In case of problems, restore a known-good `firmware.bin` via USB
with the `update` command or the full set with `install`. The old slot and
saved configuration do not guarantee compatibility when returning to a
previous configuration schema; before transitioning, keep a raw copy of
`appcfg`.

| Partition | Type/subtype | Address | Size |
| --- | --- | --- | --- |
| `nvs` | `data/nvs` | `0x9000` | `0x5000` |
| `otadata` | `data/ota` | `0xE000` | `0x2000` |
| `ota_0` | `app/ota_0` | `0x10000` | `0x3F0000` |
| `ota_1` | `app/ota_1` | `0x400000` | `0x3F0000` |
| `logs` | `data/0x40` | `0x7F0000` | `0x100000` |
| `appcfg` | `data/nvs` | `0xF00000` | `0x10000` |

`logs` is the raw persistent event log partition, not a filesystem. Measurement
history remains in RAM; event write limits are described in
[PERSISTENT_LOGS.md](PERSISTENT_LOGS.md). Ordinary `install`, `update`, and
`migrate` do not write or erase `logs` or `appcfg`.

A separate `erase --confirm ERASE_FLASH` command still erases **all** flash,
including both slots, `otadata`, `logs`, `nvs`, and `appcfg`; it makes no
backup. Use it only for an intentional full reset and after creating the needed
configuration copy.
