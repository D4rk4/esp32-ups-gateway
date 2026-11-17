# Connecting ESP32-POE-16MB and MOD-RS232

The target board of this build is **Olimex ESP32-POE-16MB with WROOM**, plain,
without ISO. Ethernet configuration cannot be automatically transferred to
WROVER, ESP32-POE2, or another board.

## UEXT connector

Connect MOD-RS232 with the standard 10-wire UEXT cable, observing the key of
the connector and pin 1 position.

| UEXT | ESP32-POE signal | Purpose |
| --- | --- | --- |
| 1 | 3.3 V | MOD-RS232 power |
| 2 | GND | Common wire |
| 3 | GPIO4, TX | Transmission ESP32 to converter |
| 4 | GPIO36, RX | Reception ESP32 from converter |

The firmware uses `Serial2` with explicit RX=36, TX=4 assignment; the hardware
UART number does not change the UEXT pinout. On the ESP32 side, signals are at
3.3 V level. RS232 connects **to the module's DB9**, not directly to GPIO.

Ethernet parameters are already set in the project: PHY LAN87xx via the LAN8720
driver, address 0, MDC=23, MDIO=18, PHY power=12, clock output GPIO17. See
[the official Olimex example](https://github.com/OLIMEX/ESP32-POE/blob/master/SOFTWARE/ESPHOME/esp32-poe.yaml)
and [the general ESP32-POE/ISO manual](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/resources/ESP32-POE-user-manual.pdf).

## MOD-RS232: signal direction

Per [the MOD-RS232 Rev A schematic](https://www.olimex.com/Products/Modules/Interface/MOD-RS232/resources/MOD-RS232_Rev_A_sch.pdf)
the standard jumpers are: `3_TX` closed / `3_RX` open; `4_RX` closed / `4_TX`
open. In this position the module has DCE assignment:

| DB9 MOD-RS232 | Direction relative to gateway |
| --- | --- |
| 2 | Output TX from ESP32 |
| 3 | Input RX to ESP32 |
| 5 | GND |

The `PC_RXD` and `PC_TXD` designations on the schematic refer to the computer
side. They do not mean that pin 2 is an input of the MOD-RS232 itself.

## Cable to Inform Sinus EVO

Per [the official Inform Sinus EVO 1–3 kVA manual](https://www.inform.com.tr/kilavuzlar/sinusevo1-3kVA-tr.pdf),
PDF page 44 / printed 39, section Ek-2, the UPS contacts are: 2=TXD output,
3=RXD input, 5=GND. For **this documented revision** and MOD-RS232 with the
standard jumpers the result is:

| MOD-RS232 | Sinus EVO |
| --- | --- |
| DB9 2, TX → | DB9 3, RX |
| DB9 3, RX ← | DB9 2, TX |
| DB9 5 | DB9 5 |

A three-wire cross 2↔3, 5↔5 is needed. The connector genders are chosen based
on the actual sockets. The plain straight PC↔UPS cable from the manual does
not compensate for the DCE pinout of MOD-RS232. Before making the cable,
check the manual and the revision of your UPS; do not connect other DB9 pins
based on assumptions. The old UPS pinout 9/6/7 from the MegaTec document does
not apply to Sinus EVO.

## Power and firmware

**Before USB flashing of a regular ESP32-POE, disconnect Ethernet with PoE.**
This is a direct instruction from the board manufacturer: ESP32-POE has no
galvanic isolation of PoE, simultaneous connection to a computer's USB can
damage equipment. During USB flashing, also disconnect the RS232 cable from
the UPS. After disconnecting USB, restore the working connections.

MOD-RS232 is a MAX3232 level converter, not an isolator. It connects signal
grounds. For a permanent connection, consider isolation and grounding of the
UPS serial port; if necessary, an isolated RS232 interface or an appropriate
isolated hardware circuit is used.

## First power-on

1. Flash the board per the main README, leaving PoE and the UPS cable
   disconnected.
2. Remove USB. Connect UEXT, the verified RS232 cable, and the working network
   / PoE.
3. Find the gateway address via DHCP and open the web interface.
4. The initial profile is `megatec_q1`, 2400 baud, 8N1.
5. Leave both control permissions disabled. Check correct Q1 responses,
   voltage, frequency, load, and power indication changes.
6. If there are no responses, first check the 2/3 direction, ground, port
   settings, and UPS documentation. Lack of connection is not a reason to
   try off commands.
7. `battery_multiplier` defaults to 0: the whole battery chain voltage is not
   published in SNMP. Choose the multiplier only after checking the meaning of
   the raw voltage: 1 to measure the whole chain, or a confirmed cell count for
   a per-cell value. The number of 12 V blocks and the number of 2 V cells
   differ.

Physical connection, Ethernet, and the responses of your UPS instance were not
verified on the development stand.
