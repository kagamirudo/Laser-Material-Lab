# Laser Material Lab — ESP32-S3 Data Logger

High-speed ADC data acquisition system for laser/material interaction studies using ESP32-S3. Captures continuous ADC samples from a photodiode, writes time-based CSV chunks to SD card, and streams them to a client over Wi-Fi.

## Hardware Setup

### Wiring

- **Laser module**
  - VCC → ESP32-S3 3V3
  - GND → ESP32-S3 GND
  - Signal (laser enable) → **GPIO5** (PWM @ 5 kHz)

- **Photodiode / phototransistor**
  - VCC → ESP32-S3 3V3
  - GND → ESP32-S3 GND
  - Analog output → **GPIO4** (ADC1_CH3)

- **SD card** (SPI) — primary storage for CSV chunks
- **OLED display** (I2C, optional) — shows IP address and status

> GPIO44 is *not* ADC-capable on ESP32-S3. Use GPIO4 or another ADC1 pin.

## Features

- **Continuous ADC sampling** at 3000 Hz (configurable, minimum 611 Hz)
- **Time-based chunked logging**: recording is split into fixed-duration CSV chunks (default 50 s) written to SD card
- **Adaptive queue-based pause**: writer only pauses between chunks when the sample queue is under pressure (>50% full), instead of a fixed delay
- **SD card primary, SPIFFS fallback**: CSV files go to SD when mounted, internal flash otherwise
- **OLED display**: shows Wi-Fi status and IP address on boot
- **SNTP time sync**: file timestamps are correct when connected to the internet (STA mode)
- **Web-based control**: HTTP server for start/stop, live ADC display, and chunk download
- **Dual-core RTOS architecture**:
  - Core 1: ADC sampling task (priority 5)
  - Core 0: CSV writer task (priority 3), HTTP server, Wi-Fi
- **Bench mode**: fixed-duration ADC capture to a single CSV or compact binary file for throughput testing

## Configuration

Key constants in `main/002.c`:

| Constant | Default | Description |
|---|---|---|
| `SAMPLE_RATE_HZ` | 3000 | Target ADC sample rate (Hz) |
| `CHUNK_CONTINUOUS_SECS` | 50 | Seconds per CSV chunk |
| `CSV_QUEUE_SIZE` | 48000 | Sample queue slots (SPIRAM, ~16 s buffer @ 3 kHz) |
| `CSV_WRITE_BATCH_SIZE` | 500 | Samples per CSV write batch |
| `SAMPLE_LIMIT` | 10000000 | Auto-stop after this many samples |

### Wi-Fi Mode

```c
#define MODE WIFI   // Connect to existing network (STA)
// or
#define MODE HOST   // Create access point (AP)
```

Wi-Fi credentials are set in `main/wifi.h`.

## Building and Flashing

### Prerequisites

- ESP-IDF v5.0+
- Python 3.8+

### Quick Start

```bash
make          # Build, flash, and monitor (default)
make build    # Build only
make flash    # Flash only
make monitor  # Monitor serial output
make bfm      # Build, flash, and monitor
make bf       # Build and flash (no monitor)
make clean    # Clean build artifacts
make menuconfig  # ESP-IDF configuration menu
make size     # Show binary size
make push MSG="your message"  # Git commit and push
```

## Usage

### 1. Power on and connect

1. Flash the firmware.
2. The OLED (if present) shows the IP address once Wi-Fi connects.
3. Open a browser and navigate to the displayed IP.

### 2. Recording (chunked mode)

1. Press **Start** in the web UI.
2. The firmware begins continuous ADC sampling and writes CSV chunks to SD.
3. Each chunk is announced as ready; the client downloads it automatically.
4. Press **Stop** to end recording. The current partial chunk is saved and downloadable.

### 3. CSV chunk format

Each chunk file (`0.csv`, `1.csv`, …) contains:

```csv
timestamp_us,adc_value
0,1234
333,1235
666,1236
```

- `timestamp_us`: microseconds since logging started (global, continuous across chunks).
- `adc_value`: 12-bit ADC reading (0–4095, ≈ 0–3.3 V).

### 4. Storage layout

```
/sdcard/laser/chunks/     Chunked recording (default)
/sdcard/laser/data.csv    Continuous single-file recording
/sdcard/tb/chunks/run_N/  Test bench chunked recordings
/spiffs/...               Fallback when SD card absent
```

## Architecture

### Data Flow

```
ADC hardware → adc_task (CPU1) → s_csv_queue (48k slots, SPIRAM)
                    ↓                        ↓
              Web UI (live value)    csv_writer_task (CPU0) → chunk .csv on SD
                                                                    ↓
                                                          HTTP server → client download
```

### Queue & Chunking

- The queue bridges ADC producer and CSV writer consumer. At 3000 Hz the 48,000-slot queue provides ~16 seconds of buffer.
- Chunks close on a time basis. During close/open transitions (~100–500 ms), samples accumulate in the queue and are drained into the next chunk — no data loss.
- An adaptive pause (0 / 500 / 1000 ms) is inserted between chunks only when queue fill exceeds 50%, reducing unnecessary latency at lower sample rates.

## Project Structure

```
.
├── main/
│   ├── 002.c          # ADC, chunked logging, RTOS tasks, bench mode
│   ├── server.c/h     # HTTP server, API endpoints, chunk download
│   ├── wifi.c/h       # Wi-Fi STA/AP configuration
│   ├── sdcard.c/h     # SD card SPI driver
│   ├── display.c/h    # OLED display driver
│   ├── web/           # Web UI (index.html, script.js, style.css)
│   └── certs/         # HTTPS certificates
├── tools/
│   ├── graph_csv.py   # Plot CSV data
│   └── bin2csv.py     # Convert bench binary to CSV
├── partitions.csv     # Partition table
├── Makefile           # Build shortcuts
├── agent.md           # Developer notes for test branch
└── CMakeLists.txt     # ESP-IDF build config
```

## Troubleshooting

| Symptom | Cause / Fix |
|---|---|
| "ADC buffer overflow" warnings | ADC internal buffer overrun — SD card or writer too slow; reduce sample rate or increase `max_store_buf_size` |
| "CSV queue nearly full" | Writer can't keep up; check SD card speed or reduce `SAMPLE_RATE_HZ` |
| Chunk 0 has fewer samples | Normal — ADC spin-up overhead consumes part of the first chunk window |
| Different sample counts per chunk | Normal — time-based chunking with scheduling jitter |
| "Failed to open CSV file (errno: 28)" | Storage full; SPIFFS auto-formats, or free space on SD |
| File dates show 1970 | SNTP not synced; ensure STA mode has internet access |

## License

MIT License. See `LICENSE`.
