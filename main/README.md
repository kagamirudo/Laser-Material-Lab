## Laser Logger — Simple Explanation

This folder contains the firmware that runs on a small ESP32-S3 board.
Its job is to:
- turn the laser on,
- read how bright the reflected light is with a sensor,
- save those readings into simple table files (CSV) on the SD card,
so you can open the data later in Excel, Google Sheets, or another data program.

### 1. What hardware is involved?

- **Laser**: A diode the ESP32 turns on at full brightness via PWM on GPIO5.
- **Light sensor (photodiode)**: Sits where the laser shines and produces a signal that changes with reflected light intensity.
- **ESP32-S3 board**: The "brain" that reads the sensor, stores data, and serves a web control panel.
- **SD card**: Primary storage for CSV data files.
- **SPIFFS (internal flash)**: Backup storage used when no SD card is available.
- **OLED display** (optional): Shows Wi-Fi status and IP address on boot.
- **Wi-Fi + Web page**: Lets you connect from a phone or laptop to start/stop recording and download data.

### 2. How often are measurements taken?

The code in `002.c` configures the ESP32's ADC (analog-to-digital converter) to sample the sensor at **3000 measurements per second** (3 kHz) on GPIO4.

Think of this as the board taking 3,000 "brightness snapshots" every second, but as numbers instead of images.

### 3. What happens to each measurement?

For each raw ADC reading:
1. The firmware gets a number (0–4095) representing how bright the laser reflection is.
2. The number is placed into a **queue** (a first-in/first-out buffer with 48,000 slots stored in SPIRAM) so writing to storage does not slow down the measurements.

If the queue ever fills up, new samples are dropped rather than pausing the ADC. The firmware keeps sampling fast rather than risking timing gaps.

### 4. How does data get written to CSV files?

A dedicated **CSV writer task** continuously:
- pulls samples from the queue,
- writes them to the current chunk CSV file on the SD card.

Each line in the CSV looks like:

```
timestamp_us,adc_value
0,2048
333,2055
666,2060
```

- **`timestamp_us`**: time in microseconds since logging started. Continues across chunks (never resets mid-session).
- **`adc_value`**: the sensor reading (0–4095). Higher = more light, lower = less light.

### 5. What is chunked logging?

Instead of one giant file, recordings are split into **time-based chunks** (default: 50 seconds each):

- Chunk `0.csv` covers seconds 0–50
- Chunk `1.csv` covers seconds 50–100
- ...and so on

**Why chunks?**
- The web client downloads each finished chunk while the next one is being recorded.
- If something goes wrong, you only lose the current chunk, not the whole recording.
- Smaller files are easier to handle on the ESP32 and over Wi-Fi.

When a chunk's time window expires:
1. The current chunk file is closed and announced as ready for download.
2. The next chunk file is opened immediately.
3. Samples that accumulated in the queue during the transition are written into the new chunk — no data is lost.

**Adaptive pause**: Between chunks, the writer only pauses if the queue is filling up (>50% full). At 3000 Hz the queue has ~16 seconds of headroom, so no pause is normally needed.

### 6. Where are the CSV files stored?

The firmware chooses automatically:
- **SD card mounted** → `/sdcard/laser/chunks/` (chunked mode) or `/sdcard/laser/data.csv` (continuous mode)
- **No SD card** → `/spiffs/chunks/` or `/spiffs/data.csv`

Test bench recordings go to `/sdcard/tb/chunks/run_N/`.

### 7. How do you start and stop recording?

From the **web interface**:
1. The ESP32 connects to Wi-Fi (or creates its own hotspot).
2. Open the displayed IP address in a browser.
3. Press **Start** → the firmware begins sampling and writing chunks.
4. Press **Stop** → the current partial chunk is saved, closed, and made available for download.

When you stop mid-chunk:
- The partial chunk is saved with however many samples were collected.
- A "done" signal tells the client there are no more chunks.
- No data is silently lost.

### 8. Why don't chunks have exactly the same number of samples?

Chunks are closed based on **elapsed time**, not a fixed sample count. At 3000 Hz for 50 seconds you expect ~150,000 samples per chunk, but small variations occur due to:
- Scheduling and queue contention
- SD card write latency
- The first chunk (chunk 0) loses a fraction of its window to ADC startup, so it may have slightly fewer samples

This is normal. Typical variation is within ±1%.

### 9. What is bench mode?

For lab throughput testing, there is a bench mode that:
- Runs the laser and ADC for a fixed duration (default 30 seconds)
- Writes to either a CSV file or a compact binary file (12-byte records)

The binary file is smaller and faster to write. Use `tools/bin2csv.py` to convert it to CSV on your computer.

### 10. How do I analyze the data?

1. Remove the SD card from the device (or download chunks via the web UI).
2. Open the CSV files in Excel, Google Sheets, LibreOffice, Python, etc.
3. Plot with:
   - **X axis**: `timestamp_us` (divide by 1,000,000 for seconds)
   - **Y axis**: `adc_value`

Use `tools/graph_csv.py` for quick plotting from the command line.

### 11. Summary in everyday words

- The board shines a **constant laser** onto your sample.
- A **light sensor** watches how bright the reflection is.
- The board takes **3,000 readings every second** and saves them as numbers with timestamps.
- The data is split into **chunks** (one CSV file per 50 seconds) and stored on the **SD card**.
- You can download chunks in real time over **Wi-Fi**, or pull the SD card later.
- Open the CSV files in any spreadsheet or data program to see how the signal changes over time.
