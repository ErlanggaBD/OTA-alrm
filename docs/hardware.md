# Hardware

Target board: **ESP32 DevKit / ESP32 Dev Module, 4 MB flash**.

The firmware is written for 8 channels but only reads and alarms the
`active_sensors` count returned by the backend. Unused channels can be left
unpopulated.

---

## Pin map

| Function | GPIO | Notes |
| --- | --- | --- |
| Sensor 1 | 21 | DHT22 data (demo) |
| Sensor 2 | 22 | DHT22 data |
| Sensor 3 | 23 | DHT22 data |
| Sensor 4 | 25 | DHT22 data |
| Sensor 5 | 26 | DHT22 data |
| Sensor 6 | 27 | DHT22 data |
| Sensor 7 | 32 | DHT22 data |
| Sensor 8 | 33 | DHT22 data |
| Buzzer | 4 | Active-high by default |
| Relay | 5 | Active-low by default |
| Status LED | 2 | On-board LED, active-high |
| Reset button | 0 | `BOOT` button, active-low, hold 8 s for factory reset |

Pins 34–39 are input-only and are not used for outputs. Avoid GPIO6–11 (flash)
and GPIO1/3 (serial).

## DHT22 wiring (demo)

Each DHT22:

```
VCC  ── 3.3 V
GND  ── GND
DATA ── GPIO n
DATA ── 10 kΩ ── 3.3 V   (pull-up)
```

- Use 3.3 V, not 5 V, for the data line. DHT22 modules that accept 5 V often
  have their own regulator and level shifting; verify before connecting.
- Keep sensor cables short (< 1 m) or use shielded cable.
- The firmware enables the ESP32 internal pull-up as well, but an external
  10 kΩ pull-up is recommended.

## Outputs

### Buzzer

- Default: active-high, driven directly from GPIO4.
- For a louder buzzer, use a transistor/MOSFET driver and a flyback diode for
  magnetic buzzers.
- Change polarity with `BUZZER_ACTIVE_HIGH` in `app_config.h`.

### Relay

- Default: active-low (`RELAY_ACTIVE_LOW 1`).
- Most relay modules are active-low and include an optocoupler and driver.
- Use a separate 5 V supply for the relay coil if needed, with a common ground.
- For medical gas valves, use an appropriately rated, fail-safe relay/contactor
  and follow local safety regulations.

### Status LED

- GPIO2 is the on-board LED on most DevKit boards.
- `LED_ACTIVE_HIGH 1`.
- Patterns: solid = normal, blinking = warning/alarm, fast blink = alarm.

### Reset button

- GPIO0 is the `BOOT` button on most DevKit boards.
- Hold for 8 seconds (`RESET_HOLD_MS`) to erase NVS and reboot into the portal.
- A short press is ignored.

## Power

- ESP32 peak current can exceed 500 mA during Wi-Fi transmission.
- Use a 5 V supply rated at least 1 A, with a bulk capacitor near the board.
- DHT22 sensors draw little current, but long cables and many sensors can cause
  voltage drops. Use a star ground.
- Do not power the relay coil from the ESP32 3.3 V regulator.

## Replacing DHT22 with real gas sensors

The DHT22 driver is deliberately isolated in `sensors.c` / `sensors.cpp`:

- `sensorsBegin()` / `sensors_init()` initializes the bus.
- `readOne()` / `read_one()` fills `g_sensors[i].primary`,
  `.secondary`, `.valid`, `.error_count`, `.last_read_ms`, `.last_read_epoch`.
- `SensorState.state` is computed by the alarm engine from thresholds.

To support a real sensor:

1. Replace the read function with your sensor protocol (UART, I2C, SPI, 4–20 mA
   ADC, etc.).
2. Keep the same `SensorState` fields so the dashboard and alarm engine work.
3. Set `.valid = false` and increment `.error_count` on communication/CRC
   failure so the channel goes to `fault` after 3 errors.
4. Adjust `DEFAULT_*` thresholds and units in `app_config.h`, or set them from
   the backend per device.
5. For safety-critical gas detection, add independent hardware interlocks and
   follow the relevant standards (IEC 60601-1-8, ISO 7396-1, local codes).

## Antenna and enclosure

- Keep the antenna area clear of metal and the relay/buzzer wiring.
- Use a vented enclosure for gas sensors; do not seal the sensor behind a
  moisture trap.
- Route sensor cables away from relay switching wires.
- Add strain relief and fusing appropriate to the installation.
