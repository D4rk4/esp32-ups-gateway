# Connecting to Observium CE

Included: a local discovery profile, an example `snmptrapd` receiver and an optional notification-to-syslog adapter. These are prepared integration files; they **have not been installed or verified on your Observium CE server**. The full version of your CE is unknown. The paths below assume installation into `/opt/observium`.

## 1. SNMP polling and discovery

The firmware answers SNMPv2c, UDP/161, and exposes a subset of the APC PowerNet-MIB. `sysDescr.0` begins with `ESP32 UPS Gateway/`. `sysObjectID.0` is deliberately `0.0` (`zeroDotZero`): the project has no assigned own enterprise/model OID and does not declare itself a specific APC model. Therefore the profile is determined by the description.

The [official Observium reference](https://docs.observium.org/developing/definitions/) places OS definitions in `includes/definitions/os/*.inc.php`. Copy the supplied file:

```sh
sudo install -m 0644 integration/observium/esp32ups.inc.php /opt/observium/includes/definitions/os/esp32ups.inc.php
php -l /opt/observium/includes/definitions/os/esp32ups.inc.php
```

The file sets type `power`, the regular expression `/^ESP32 UPS Gateway\//` and a MIB list including `PowerNet-MIB`, per the [OS detection description](https://docs.observium.org/developing/add_os/). A separate `require` in `config.php` is not needed with this placement. Keep a copy of the local definition when updating/reinstalling Observium.

In the gateway web interface set the SNMP community and, if necessary, the IPv4 of the single allowed poller. Add the device in Observium with SNMPv2c and the same community. The DHCP address is best fixed with a reservation. For diagnostics, replacing all placeholders:

```sh
snmpget -v2c -c 'REPLACE_READ_COMMUNITY' REPLACE_GATEWAY_IPV4 .1.3.6.1.2.1.1.1.0 .1.3.6.1.2.1.1.2.0
snmpwalk -v2c -c 'REPLACE_READ_COMMUNITY' REPLACE_GATEWAY_IPV4 .1.3.6.1.4.1.318.1.1.1
cd /opt/observium
./discovery.php -h REPLACE_DEVICE_ID -m os -d
./discovery.php -h REPLACE_DEVICE_ID -d
./poller.php -h REPLACE_DEVICE_ID -d
```

Expected signs: OS `esp32-ups-gateway`, PowerNet-MIB connected, real voltages/load/input frequency/temperature and link/power states. First achieve correct Q1 exchange: before it the measurement OIDs are absent. Which of the supported objects your CE turns into sensors is checked by discovery output; in case of mismatch the local definition/sensors will need adapting to the installed version.

Base Q1 does not report charge in percent, runtime and measured output frequency. Their absence is expected. Battery string voltage appears after confirming `battery_multiplier`. Temperature PowerNet objects with the historical name `upsAdvBatteryTemperature` describe the internal UPS temperature; this is not a declaration of a separate battery sensor. On RS232 loss SNMP communication with the gateway stays available, `upsCommStatus` reports the absence of a link, stale measurements are not issued as fresh.

## 2. Receiving traps

This firmware version sends **SNMPv2c TRAP**, without delivery confirmation, to two configurable receivers. INFORM, v1 and v3 sending are not implemented. To receive, install the Net-SNMP `snmptrapd` package the way accepted in your OS.

Configure the receiver in the ESP32 web interface: server address, UDP port 162, a separate trap community, enabled. Allow incoming UDP/162 on the server only from the gateway address. The SNMP polling policy UDP/161 on ESP32 is configured separately.

In `snmptrapd.conf.example` replace `REPLACE_RECEIVER_IPV4`, `REPLACE_GATEWAY_IPV4` and `REPLACE_TRAP_COMMUNITY`. Do not use `disableAuthorization yes`. The `authCommunity log,execute` directive is limited to a specific source. It technically allows v1/v2c at the Net-SNMP level; the handler stdin format is converted to v2 and does not allow reliably determining the original version. Here the stream created by our v2c sender is supported, not version filtering promised by this example.

Installing the handler and a trial run of the receiver after preparing the configuration:

```sh
sudo install -d -m 0755 /usr/local/lib/esp32-ups
sudo install -m 0755 integration/observium/trap_to_syslog.py /usr/local/lib/esp32-ups/trap_to_syslog.py
sudo install -m 0600 integration/observium/snmptrapd.conf.example /etc/snmp/esp32-ups-trapd.conf
sudo snmptrapd -f -Lo -n -On -C -c /etc/snmp/esp32-ups-trapd.conf
```

Do not run a second instance on an already occupied UDP/162. After checking, move the parameters into your OS service; a universal unit file does not replace the existing configuration. `-On` / `outputOption n` are required: the handler expects numeric OIDs, independent of the local MIB set. `-n` disables reverse DNS. The Python code does not use shell/eval and passes text only to the local syslog; service configuration files must belong to the administrator and be inaccessible to the receiving process.

| Notification | Numeric OID |
|---|---|
| communicationLost | 1.3.6.1.4.1.318.0.1 |
| upsOnBattery | 1.3.6.1.4.1.318.0.5 |
| lowBattery | 1.3.6.1.4.1.318.0.7 |
| communicationEstablished | 1.3.6.1.4.1.318.0.8 |
| powerRestored | 1.3.6.1.4.1.318.0.9 |
| returnFromLowBattery | 1.3.6.1.4.1.318.0.11 |
| Gateway startup / channel test button | 1.3.6.1.6.3.1.1.5.1, coldStart |

The test notification argument `mtrapargsString.0` (`1.3.6.1.4.1.318.2.3.3.0`) begins with `TEST TRAP:`. The adapter marks it `testNotification` so a channel check does not look like a real restart. The argument also carries the boot id, sequence and uptime of the original event. A general UPS fault and selftest completion remain in the local firmware log; the selftest result is not reported by Q1.

## 3. Optional: events in Observium syslog

Path: ESP32 → snmptrapd → `trap_to_syslog.py` → `127.0.0.1:514` → rsyslog → `syslog.php`.

The adapter takes the source address from the second line of transport, checks strict IPv4 and the allowlist, then places the gateway IP in the RFC3164 HOSTNAME header. The first line of reverse DNS is ignored. Limits: stdin 16 KiB, 128 lines, 2048 bytes/line; outgoing syslog ≤1024 bytes. Unknown OIDs, duplicates of required fields, an invalid source and control characters are rejected.

**In the rsyslog template for this local path use `%hostname%`, not `%fromhost%` or `%fromhost-ip%`.** The latter two here mean the local server 127.0.0.1. The `rsyslog-esp32-ups.conf.example` example uses `%hostname%` and allows only the local transport, the needed program name and the explicitly given gateway IP.

The example is intended for a server without an existing UDP/514 listener. If the port is already served, do not create a conflicting listener and do not reload modules: add this template and filter to the corresponding ruleset of the existing input. An alternative is to allocate another loopback port and equally change it in the input and the handler `--syslog-port` argument.

In Observium enable syslog in `config.php`:

```php
$config['enable_syslog'] = 1;
```

If the gateway IP did not map to the device, set the address correspondence to its real device id, for example for the **documentation address** and a conditional id 123:

```php
$config['syslog']['host_map']['192.0.2.50'] = 123;
```

Do not substitute 123 without checking. After changing rsyslog check `rsyslogd -N1`, the rights to run `/opt/observium/syslog.php`, service/AppArmor restrictions and then apply the configuration with a normal service restart. Do not disable system-wide restrictions just for this example. More: [official syslog integration](https://docs.observium.org/syslog/).

RFC3164 time is the receiver's local time. The event time from the ESP32 side remains uptime in the varbind/message; the adapter does not issue it as absolute UTC time. UDP delivery in both segments is not confirmed. `sent` in the ESP32 counter means handing the datagram to the network stack, not the appearance of a line in Observium.

Syslog Alerting in the [Observium documentation](https://docs.observium.org/alerting_syslog/) is marked Subscription-only. Displaying the log in CE does not promise email/webhook notifications. A separate external notifier is not included.

## 4. Verification without network and bench acceptance

The synthetic file uses TEST-NET addresses, there are no real UPS events in it:

```sh
python3 integration/observium/trap_to_syslog.py --allow-source 192.0.2.50 --dry-run < integration/observium/example-trap.txt
python3 -m unittest discover -s tests -p test_trap_adapter.py -v
```

These commands do not send UDP. After setting up the server press the test trap in the web interface and check first the receiver, then the syslog of the gateway card specifically. Then check RS232 break/restoration and transition to battery according to your bench procedure. Do not create a low-battery discharge of critical load just to check the channel.

The stdin/traphandle format and authorization: [official snmptrapd.conf](https://www.net-snmp.org/docs/man/snmptrapd.conf.html). The script is designed for a modern second line `UDP: [IPv4]:port->[IPv4]:port`; old Net-SNMP, printing only the IP, will be rejected with diagnostics until explicit parser adaptation.
