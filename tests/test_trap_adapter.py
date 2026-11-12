"""Behavior tests: source spoofing, misleading event names, and log injection."""
import datetime as dt
import importlib.util
import io
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "integration/observium/trap_to_syslog.py"
spec = importlib.util.spec_from_file_location("trap_to_syslog", ADAPTER)
adapter = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = adapter
spec.loader.exec_module(adapter)


def sample(oid=".1.3.6.1.4.1.318.0.5", detail="On battery; boot_id=7; seq=8", source="192.0.2.50"):
    return (
        "misleading-reverse-dns.example\n"
        f"UDP: [{source}]:42000->[192.0.2.10]:162\n"
        ".1.3.6.1.2.1.1.3.0 0:0:00:01.23\n"
        f".1.3.6.1.6.3.1.1.4.1.0 {oid}\n"
        f'.1.3.6.1.4.1.318.2.3.3.0 STRING: "{detail}"\n'
        '.1.3.6.1.2.1.1.5.0 STRING: "ups-gateway"\n'
    ).encode("utf-8")


class TrapAdapterTests(unittest.TestCase):
    def parse(self, data):
        return adapter.parse_notification(data, ["192.0.2.50"])

    def test_source_comes_from_transport_not_reverse_dns(self):
        event = self.parse(sample())
        packet = adapter.format_syslog(event, dt.datetime(2026, 11, 11, 9, 8, 7))
        self.assertEqual(
            packet,
            b"<172>Nov 11 09:08:07 192.0.2.50 esp32-ups-trap: "
            b"upsOnBattery oid=1.3.6.1.4.1.318.0.5 "
            b"uptime=0:0:00:01.23 On battery; boot_id=7; seq=8",
        )

    def test_known_fault_and_recovery_types_have_distinct_severity(self):
        cases = [
            (1, "communicationLost", 3), (5, "upsOnBattery", 4),
            (7, "lowBattery", 2), (8, "communicationEstablished", 5),
            (9, "powerRestored", 5), (11, "returnFromLowBattery", 5),
        ]
        for number, name, severity in cases:
            with self.subTest(number=number):
                event = self.parse(sample(f".1.3.6.1.4.1.318.0.{number}"))
                self.assertEqual((event.name, event.severity), (name, severity))

    def test_test_button_is_not_reported_as_actual_startup(self):
        test = self.parse(sample(".1.3.6.1.6.3.1.1.5.1", "TEST TRAP: channel test"))
        startup = self.parse(sample(".1.3.6.1.6.3.1.1.5.1", "Gateway started"))
        self.assertEqual((test.name, test.severity), ("testNotification", 6))
        self.assertEqual((startup.name, startup.severity), ("coldStart", 5))

    def test_oid_type_prefix_and_equals_are_supported(self):
        data = sample()
        data = data.replace(b".1.4.1.0 .1.3.6", b".1.4.1.0 = OID: .1.3.6")
        self.assertEqual(self.parse(data).name, "upsOnBattery")

    def test_hex_utf8_argument_is_decoded_as_text(self):
        data = sample(detail="text").replace(b'STRING: "text"', b"Hex-STRING: D0 A2 D0 B5 D1 81 D1 82")
        self.assertEqual(self.parse(data).detail, "Тест")

    def test_wrapped_hex_argument_is_decoded_without_accepting_free_text_lines(self):
        data = sample(detail="text").replace(b'STRING: "text"', b"Hex-STRING: D0 A2 D0 B5\n D1 81 D1 82")
        self.assertEqual(self.parse(data).detail, "Тест")
        with self.assertRaises(ValueError):
            self.parse(sample(detail="text").replace(b'STRING: "text"', b'"text"\n D1 81'))

    def test_observium_column_separator_cannot_be_injected_in_message(self):
        event = self.parse(sample(detail="UPS text||fake field|||third||||fourth"))
        packet = adapter.format_syslog(event, dt.datetime(2026, 11, 11, 9, 8, 7))
        self.assertNotIn(b"||", packet)
        self.assertIn(b"UPS text| |fake field", packet)
        self.assertIn(b"field| | |third| | | |fourth", packet)

    def test_unknown_source_is_rejected_even_if_hostname_is_allowed_ip(self):
        data = sample(source="192.0.2.99").replace(b"misleading-reverse-dns.example", b"192.0.2.50")
        with self.assertRaises(ValueError):
            self.parse(data)

    def test_malformed_or_non_ipv4_transport_is_rejected(self):
        for transport in [
            b"UDP: [192.0.2.50evil]:42000->[192.0.2.10]:162",
            b"UDP: [192.0.2.50]:99999->[192.0.2.10]:162",
            b"UDP: [999.1.2.3]:42000->[192.0.2.10]:162",
            b"UDP: [192.0.2.50]:42000->[192.0.2.10]:162 trailing",
            b"UDP: [::ffff:192.0.2.50]:42000->[::1]:162",
            b"TCP: [192.0.2.50]:42000->[192.0.2.10]:162",
        ]:
            with self.subTest(transport=transport), self.assertRaises(ValueError):
                self.parse(sample().replace(sample().splitlines()[1], transport))

    def test_missing_duplicate_unknown_or_non_numeric_trap_is_rejected(self):
        malformed = [
            sample(".1.3.6.1.4.1.318.0.999"),
            sample("PowerNet-MIB::upsOnBattery"),
            sample().replace(b".1.3.6.1.2.1.1.3.0 0:0:00:01.23\n", b""),
            sample() + b".1.3.6.1.6.3.1.1.4.1.0 .1.3.6.1.4.1.318.0.9\n",
        ]
        for data in malformed:
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.parse(data)

    def test_log_injection_and_invalid_utf8_are_rejected(self):
        for data in [sample(detail="hello\n<0>forged"), sample(detail="hi\x00there"), sample()+b"\xff"]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.parse(data)

    def test_bounded_input_is_rejected_not_partially_parsed(self):
        data = sample() + b"x" * (16 * 1024)
        with self.assertRaises(ValueError):
            adapter.read_notification(io.BytesIO(data), ["192.0.2.50"])

    def test_output_stays_single_line_and_bounded_in_bytes(self):
        event = self.parse(sample(detail="Ж" * 450))
        packet = adapter.format_syslog(event, dt.datetime(2026, 1, 2, 3, 4, 5))
        self.assertTrue(packet.startswith(b"<172>Jan  2 03:04:05 192.0.2.50 "))
        self.assertLessEqual(len(packet), 1024)
        self.assertNotIn(b"\n", packet)
        packet.decode("utf-8")

    def test_dry_run_cli_emits_formatted_message_without_network(self):
        result = subprocess.run(
            [sys.executable, str(ADAPTER), "--allow-source", "192.0.2.50", "--dry-run"],
            input=sample(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"192.0.2.50 esp32-ups-trap: upsOnBattery", result.stdout)


if __name__ == "__main__":
    unittest.main()
