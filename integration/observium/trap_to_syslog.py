#!/usr/bin/env python3
"""Bounded Net-SNMP traphandle adapter for this firmware's SNMPv2c events.

No third-party packages. The UDP transport address, independently allowlisted,
becomes the RFC3164 HOSTNAME so Observium can associate the originating device.
The reverse-DNS first input line and sysName varbind are never trusted as source.
"""
import argparse
import datetime as dt
import ipaddress
import re
import socket
import sys
from dataclasses import dataclass
from typing import BinaryIO, Iterable

MAX_INPUT = 16 * 1024
MAX_LINE = 2048
MAX_LINES = 128
MAX_SYSLOG = 1024
UPTIME_OID = "1.3.6.1.2.1.1.3.0"
TRAP_OID = "1.3.6.1.6.3.1.1.4.1.0"
DETAIL_OID = "1.3.6.1.4.1.318.2.3.3.0"
COLD_START = "1.3.6.1.6.3.1.1.5.1"
EVENTS = {
    "1.3.6.1.4.1.318.0.1": ("communicationLost", 3),
    "1.3.6.1.4.1.318.0.5": ("upsOnBattery", 4),
    "1.3.6.1.4.1.318.0.7": ("lowBattery", 2),
    "1.3.6.1.4.1.318.0.8": ("communicationEstablished", 5),
    "1.3.6.1.4.1.318.0.9": ("powerRestored", 5),
    "1.3.6.1.4.1.318.0.11": ("returnFromLowBattery", 5),
    COLD_START: ("coldStart", 5),
}
TRANSPORT = re.compile(
    r"UDP:\s*\[([0-9.]+)\]:(\d{1,5})->\[([0-9.]+)\]:(\d{1,5})"
)
VARBIND = re.compile(r"(\.?[0-9]+(?:\.[0-9]+)+)\s+(?:=\s*)?(.+)")


@dataclass(frozen=True)
class Notification:
    source: str
    oid: str
    name: str
    severity: int
    uptime: str
    detail: str


def _oid(value: str) -> str:
    if value.startswith("OID:"):
        value = value[4:].strip()
    if not re.fullmatch(r"\.?[0-9]+(?:\.[0-9]+)+", value):
        raise ValueError("numeric OID required; run snmptrapd with -On")
    parts = value.lstrip(".").split(".")
    if len(parts) > 64 or any(len(part) > 10 for part in parts):
        raise ValueError("OID exceeds supported bounds")
    numbers = [int(part) for part in parts]
    if (numbers[0] > 2 or (numbers[0] < 2 and numbers[1] > 39)
            or any(number > 0xFFFFFFFF for number in numbers)):
        raise ValueError("invalid numeric OID")
    return ".".join(str(number) for number in numbers)


def _detail(value: str) -> str:
    if value.startswith("Hex-STRING:"):
        hex_text = value[len("Hex-STRING:"):].strip()
        if not re.fullmatch(r"[0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})*", hex_text):
            raise ValueError("invalid hexadecimal argument")
        try:
            value = bytes.fromhex(hex_text).decode("utf-8")
        except UnicodeError as error:
            raise ValueError("argument is not UTF-8") from error
    elif value.startswith("STRING:"):
        value = value[len("STRING:"):].strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    # Reject actual line/control bytes even when delivered in Hex-STRING form.
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError("control characters in argument")
    return " ".join(value.split())


def parse_notification(payload: bytes, allowed_sources: Iterable[str]) -> Notification:
    if not payload or len(payload) > MAX_INPUT:
        raise ValueError("notification exceeds input bounds")
    if any(byte < 32 and byte not in (10, 13) for byte in payload) or b"\x7f" in payload:
        raise ValueError("control characters in notification")
    try:
        text = payload.decode("utf-8")
    except UnicodeError as error:
        raise ValueError("notification is not UTF-8") from error
    lines = text.splitlines()
    if len(lines) < 4 or len(lines) > MAX_LINES:
        raise ValueError("unexpected notification line count")
    if any(not line or len(line.encode("utf-8")) > MAX_LINE for line in lines):
        raise ValueError("empty or overlong notification line")
    transport = TRANSPORT.fullmatch(lines[1])
    if not transport:
        raise ValueError("expected IPv4 UDP transport address")
    try:
        source = str(ipaddress.IPv4Address(transport[1]))
        ipaddress.IPv4Address(transport[3])
        allowed = {str(ipaddress.IPv4Address(item)) for item in allowed_sources}
    except ipaddress.AddressValueError as error:
        raise ValueError("invalid IPv4 address") from error
    if not (1 <= int(transport[2]) <= 65535 and 1 <= int(transport[4]) <= 65535):
        raise ValueError("invalid UDP port")
    if source not in allowed:
        raise ValueError("source is not allowlisted")
    values = {}
    order = []
    for line in lines[2:]:
        match = VARBIND.fullmatch(line)
        if not match:
            # Net-SNMP can wrap a hexadecimal OCTET STRING across lines.
            # A continuation is accepted only for a preceding Hex-STRING
            # value and only when every token is exactly one hexadecimal byte.
            if (order and values[order[-1]].startswith("Hex-STRING:")
                    and re.fullmatch(r"\s*[0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})*\s*", line)):
                values[order[-1]] += " " + line.strip()
                continue
            raise ValueError("expected numeric varbind; run snmptrapd with -On")
        key = _oid(match[1])
        if key in values:
            raise ValueError("duplicate varbind")
        values[key] = match[2].strip()
        order.append(key)
    if order[:2] != [UPTIME_OID, TRAP_OID]:
        raise ValueError("missing or misplaced SNMPv2 notification fields")
    oid = _oid(values[TRAP_OID])
    if oid not in EVENTS:
        raise ValueError("unsupported notification OID")
    if DETAIL_OID not in values:
        raise ValueError("missing firmware event argument")
    detail = _detail(values[DETAIL_OID])
    name, severity = EVENTS[oid]
    if oid == COLD_START and detail.startswith("TEST TRAP:"):
        name, severity = "testNotification", 6
    uptime = " ".join(values[UPTIME_OID].split())
    if len(uptime) > 120:
        raise ValueError("overlong uptime value")
    return Notification(source, oid, name, severity, uptime, detail)


def read_notification(stream: BinaryIO, allowed_sources: Iterable[str]) -> Notification:
    # Read only MAX_INPUT+1, so an oversized input cannot grow memory unboundedly.
    return parse_notification(stream.read(MAX_INPUT + 1), allowed_sources)


def format_syslog(event: Notification, now: dt.datetime) -> bytes:
    month = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")[now.month - 1]
    stamp = f"{month} {now.day:2d} {now.hour:02d}:{now.minute:02d}:{now.second:02d}"
    priority = 21 * 8 + event.severity  # local5 facility
    header = f"<{priority}>{stamp} {event.source} esp32-ups-trap: "
    body = f"{event.name} oid={event.oid} uptime={event.uptime} {event.detail}"
    # Observium's syslog.php input uses || as a column separator. Neutralize
    # it in event text before rsyslog applies that template.
    body = re.sub(r"\|{2,}", lambda match: " ".join(match[0]), body)
    packet = (header + body).encode("utf-8")
    if len(packet) > MAX_SYSLOG:
        packet = packet[:MAX_SYSLOG - 3].decode("utf-8", errors="ignore").encode("utf-8") + b"..."
    return packet


def _port(value: str) -> int:
    number = int(value)
    if not 1 <= number <= 65535:
        raise argparse.ArgumentTypeError("port must be 1..65535")
    return number


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-source", action="append", required=True, type=ipaddress.IPv4Address,
                        help="gateway IPv4; repeat for more than one explicitly allowed gateway")
    parser.add_argument("--syslog-port", default=514, type=_port,
                        help="local 127.0.0.1 UDP syslog port (default: 514)")
    parser.add_argument("--dry-run", action="store_true", help="print formatted record; send no UDP packet")
    args = parser.parse_args(argv)
    try:
        event = read_notification(sys.stdin.buffer, (str(item) for item in args.allow_source))
        packet = format_syslog(event, dt.datetime.now())
        if args.dry_run:
            sys.stdout.buffer.write(packet + b"\n")
        else:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
                receiver.settimeout(1.0)
                receiver.sendto(packet, ("127.0.0.1", args.syslog_port))
        return 0
    except (ValueError, OSError) as error:
        print(f"trap_to_syslog: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
