## Laser + Light Sensor (ESP32-S3) Setup

This project drives a laser on the ESP32-S3 with PWM and reads a matching light sensor/receiver using the ADC, adapted from an Arduino UNO sketch.

### Wiring

- **Laser module**
  - **VCC** → ESP32-S3 **3V3**
  - **GND** → ESP32-S3 **GND**
  - **Signal (laser enable)** → **GPIO5**

- **Light sensor / receiver (photodiode / phototransistor module)**
  - **VCC** → ESP32-S3 **3V3**
  - **GND** → ESP32-S3 **GND**
  - **Analog output** → **GPIO4** (ADC1_CH3 on ESP32-S3)

> GPIO44 is *not* ADC‑capable on ESP32‑S3; use GPIO4 (or another ADC pin) for the analog signal.

### Firmware behavior (current `main/002.c`)

- Configures **LEDC PWM** on **GPIO5** at 5 kHz, 8‑bit resolution, **full duty** (equivalent to `analogWrite(laserPin, 255)` on Arduino).
- Configures **ADC oneshot** on **GPIO4** (auto‑mapped to the correct ADC unit/channel, 12‑bit, 0–3.3 V range).
- In a loop:
  - Takes **16 ADC samples**, averages them to reduce noise.
  - Prints `ADC raw=<value>` every 100 ms via UART.
  - When the averaged value is in the **1400–2000** range (your observed “beam hitting receiver” band), it prints `ADC raw=<value> [BEAM DETECTED]`.

### Using it

1. Wire laser and sensor exactly as above.
2. Point the laser so the beam can hit the sensor.
3. Build/flash/monitor with `idf.py build flash monitor`.
4. Watch the serial output:
   - Baseline / ambient: values around **200–500**.
   - Beam on receiver: values around **1400–2000**, logged with **`[BEAM DETECTED]`**.

You can adjust the detection range in `main/002.c` by changing `BEAM_DETECTED_MIN` and `BEAM_DETECTED_MAX`.

