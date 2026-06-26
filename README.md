# Love Lamp

A pair of touch-activated lamps that light up together over WiFi. Tap one lamp to turn both on in a chosen color; tap again to turn them off.

<p align="center">
  <img src="IMG_9857.jpg" alt="Love Lamp" width="35%">
</p>

## Hardware

- WEMOS ESP32 D1 Mini
- TTP223 capacitive touch sensor
- WS2812B 12-LED ring
- 3D-printed enclosure (from [this project](https://www.instructables.com/Love-Lamp-1))

**Wiring**

| Component | Connection |
|-----------|------------|
| TTP223 VCC | 3.3V |
| TTP223 SIG | IO23 (D7) |
| LED VCC | 5V |
| LED DIN | IO17 (D3) |
| GND | Shared |

Build two lamps — one flashed as **A**, one as **B**.

## Firmware

```bash
cd firmware
cp src/secrets.h.example src/secrets.h   # add HiveMQ Cloud credentials
pio run -e wemos_d1_mini32 -t upload     # Lamp A
pio run -e wemos_d1_mini32_B -t upload   # Lamp B
```

Lamps communicate over MQTT (`lovelamp/event` and `lovelamp/ack` topics).

## Gestures

| Action | Result |
|--------|--------|
| Single tap | Cycle color and turn on (white → blue → pink → red → off) |
| Triple tap | Enter WiFi setup portal (SSID: `Z+S`) |

Lamps auto-off after 30 minutes.
