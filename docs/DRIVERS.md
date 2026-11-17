# Serial protocol drivers

This version implements one profile: `megatec_q1`. The driver implements the
documented MegaTec family; the KPM220 binary code is not used.

## Files and boundaries

| File | Responsibility |
| --- | --- |
| `lib/UpsCore/src/ups_model.hpp` | Normalized state, validity flags, commands, UART parameters, and capabilities |
| `lib/UpsCore/src/ups_driver.hpp` | Abstract `UpsDriver` contract |
| `lib/UpsCore/src/megatec_q1.hpp/.cpp` | Building Q1/F/I, defining the reply boundary, MegaTec parsing, command encoding |
| `lib/UpsCore/src/driver_registry.hpp/.cpp` | Driver list and lookup by stable id |
| `src/ups_service.cpp` | Sole owner of UART, timeouts, freshness, management queue |
| `lib/GatewayCore/src/powernet_mib.cpp` | Representing normalized values through PowerNet |

`UpsCore` does not depend on Arduino, the web server, or UDP and is tested with
a normal C++17 compiler.

## Contract

- `info()` returns immutable `DriverInfo`: id, label, default_baud,
  capabilities, parity, stop_bits. The UART data format is currently fixed as 8
  bits; None/Even/Odd and 1/2 stop bits are supported. The working speed is
  taken from the saved `Config::baud`, so on a profile change it must be
  configured separately.
- `buildPoll(PollKind, bytes)` builds the complete request, including the
  terminator or checksum. A missing optional Rating/Identity returns false and
  empty bytes.
- `maxReplyLength()` limits the buffer to one reply, including the terminator.
  It must be finite and sufficient for the protocol.
- `frameComplete(kind, buffer, idle_ms)` belongs to the driver. UART passes the
  raw bytes without removing NUL, CR, or high bits. Therefore a future driver
  can define a frame by length, terminator, header, or pause, and then check
  the CRC in the parser. This is an interface capability, not an
  implementation of a new protocol.
- `parseReply(kind, line, state)` returns Accepted, Ignored, or Invalid. On
  Invalid/Ignored the state must remain unchanged. Parsing is performed into a
  copy with fixing only after the whole frame is checked.
- `encodeCommand(command, args, out, error)` checks capabilities/ranges and
  builds the bytes. The `changes_output` field includes the additional power
  operation permission. `settle_ms` sets the pause before the next poll.

The driver does not change `connected`, `last_success_ms`, the error/success
poll counters, and does not send network events. This data belongs to
`UpsService`. Success of F/I does not extend the freshness of Q1. After loss of
connection, SNMP publishes the connection/unknown state and excludes stale
measurements.

The current command contract confirms only the transfer of bytes: the result in
the web interface is `sent_unconfirmed`. Responses to management commands are
not interpreted as a separate ACK transaction. For a future protocol requiring
command confirmation or a multi-step handshake, the contract and `UpsService`
must be extended, not issuing the fact of UART writing as a completed
operation.

## What the MegaTec Q1 profile can do

Q1 is used for state, F for ratings, I for identification. The ASCII frame
boundary is CR or LF; the reply limit is 160 bytes. The fields of I have fixed
width; prefixes `#` and `!` are accepted and three layouts (length without CR):
33 — 10 characters each of manufacturer, model, and version; 38 — 15/10/10;
60 — 15/10/10 and 21 serial number characters. The layouts are taken from the
KSTAR card analysis, see [KSTAR_PROTOCOL.md](KSTAR_PROTOCOL.md). Request echoes
and recognized NAK are ignored; corrupted/truncated lines are rejected without
partial update. Markers of a missing value from `@` reset the validity of the
corresponding measurement.

Input and alarm input voltage, output voltage, load in percent, input
frequency, raw battery voltage, UPS temperature, and status bits are
normalized. The bypass/AVR bit means Bypass only for the online topology. The
shutdown-active flag does not mean the output is already off.

Q1 does not provide charge percent, remaining time, measured output frequency,
self-test result, or battery replacement flag; the serial number is present
only in the 60-character variant of I. This version does not invent them.
General fault and test completion fall into the local log, but do not turn into
an unconfirmed APC trap "test failed/passed".

`upsAdvBatteryTemperature` / its high-precision counterpart per the PowerNet
description mean the internal UPS temperature, despite the word Battery in the
historical name. Therefore `ups_temperature` is correctly mapped in these
objects. A separate `battery_temperature` in the model is kept for a protocol
that really reports battery temperature. See the object description in
[APC PowerNet-MIB](https://raw.githubusercontent.com/librenms/librenms/master/mibs/apc/PowerNet-MIB).

The battery multiplier is applied by the service after parsing the raw value; 0
means "scale not confirmed". It is forbidden to replace a missing runtime with
zero or to copy the nominal F frequency into the measured output frequency.

Management: toggling the beeper (`Q`), short test (`T`), test to battery
discharge (`TL`), test for 1–99 minutes (`Tnn`), cancel test (`CT`), cancel
shutdown (`C`), delayed shutdown (`Snn`, `S.n`), and restart output
(`SnnRmmmm`, `S.nRmmmm`). The shutdown delay is 1–10 minutes or 12–54 seconds
in steps of 6 (`S.2`–`S.9`, only for a manual command; schedules work in
minutes); restoration is after 3–9999 minutes. All names, labels, capability
bits, and the command confirmation class are set by one table in `UpsCore`.
Both management permissions are disabled by default. Command support by the
protocol family is not a stand check of a specific UPS.

Behavior sources: [MegaTec description](https://networkupstools.org/protocols/megatec.html),
[NUT blazer.c](https://github.com/networkupstools/nut/blob/master/drivers/blazer.c).
The project implementation is written independently per the protocol.

## Adding a confirmed dialect

1. Save anonymized real replies and the protocol description. Separately define
   fields, units, CRC/boundaries, optional replies, and management semantics.
2. Add a derived class in `lib/UpsCore/src/`. Do not change the existing MegaTec
   for incompatible packets.
3. First add host tests in `tests/test_<driver>.cpp`: correct values, each bit,
   partial frames, wrong CRC/length, echo, timeout boundary, unknown values,
   refusal atomicity, and exact management bytes.
4. Implement all `UpsDriver` methods. Mark `Metric::valid=true` only for data
   whose meaning is established. Do not reuse an existing flag for a similar-in-
   name but different fault.
5. Register a static instance in `drivers()` in `driver_registry.cpp`; do not
   rename the id after release, it is stored in the configuration. The profile
   automatically appears in the web interface list.
6. If additional fields, commands, or events are needed, consistently extend the
   model, service, web API, PowerNet mapping, and tests. Do not invent an APC
   OID for a missing semantics.
7. Run `python3 tools/test_host.py` after installing the project dependencies;
   then `pio run -e esp32-poe-16mb`. Host tests do not replace stand checks of
   UART, timings, and management functions.

The driver is configured before the service starts. Changed settings are applied
after a gateway reboot. Re-initialization of UART during another SNMP/HTTP
request is not performed.
