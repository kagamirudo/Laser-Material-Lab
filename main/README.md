## Laser Logger – Simple Explanation

This folder contains the firmware that runs on a small ESP32‑S3 board.  
Its job is to:
- turn the laser on,
- read how bright the reflected light is with a sensor,
- save those readings into a simple table file (CSV) on the SD card,  
so you can open the data later in Excel, Google Sheets, or another data program.

### 1. What hardware is involved?

- **Laser**: A diode the ESP32 turns on with a steady brightness.
- **Light sensor (photodiode)**: Sits where the laser shines, and produces a tiny electrical signal that changes with the reflected light.
- **ESP32‑S3 board**: The “brain” that:
  - reads the sensor,
  - stores data,
  - serves a small web page you can use as a control panel.
- **SD card**: Where the data files are stored when present.
- **SPIFFS (internal flash)**: Backup storage inside the ESP32, used when no SD card is available.
- **Wi‑Fi + Web page**: Lets you connect from a phone or laptop and start/stop logging.

### 2. How often are measurements taken?

The code in `002.c` configures the ESP32’s **ADC** (analog‑to‑digital converter) to:
- sample the sensor at about **4000 measurements per second** (4 kHz),
- use only one input pin (`GPIO4`) for the sensor.

You can think of this as the board taking **4,000 “brightness photos” every second**, but in numbers instead of images.

### 3. What happens to each measurement?

For each raw measurement coming from the sensor:
- The ADC produces a number (the **ADC value**) that represents how bright the laser reflection is at that moment.
- The firmware keeps track of how many samples have been taken.
- When logging is enabled, each sample is:
  - packaged with a **timestamp** (time in microseconds since logging started), and
  - placed into a **queue** (a first‑in/first‑out buffer) so writing to storage does not slow down the measurements.

If the queue ever fills up, a few samples may be dropped. The firmware prefers to **keep sampling fast** rather than pause and risk timing gaps.

### 4. How does the data get written to a CSV file?

There is a dedicated **CSV writer task** that:
- waits for samples to appear in the queue,
- collects them in a **batch** (for example 500 samples at a time),
- writes the batch to the CSV file as text lines.

Each line in the CSV looks like:

```text
timestamp_us,adc_value
12345,2048
12600,2055
...
```

- **`timestamp_us`**: time in microseconds from the moment logging started.  
  - Example: `1000000` means 1 second after logging began.
- **`adc_value`**: the sensor reading. Higher numbers mean more light, lower numbers mean less light.

The writer:
- writes regularly to avoid losing data,
- but **flushes** (forces data to storage) only every few batches, to reduce wear on the SD card / flash.

### 5. Where is the CSV stored?

The firmware chooses the storage location automatically:
- **If SD card is mounted**:
  - Directory: a `laser` folder on the SD card.
  - File name: `data.csv` inside that folder.
- **If no SD card is present**:
  - Storage: **SPIFFS**, the ESP32’s internal flash filesystem.
  - File path: `/spiffs/data.csv`.

You do **not** need to worry about paths when using the device:
- the firmware checks whether the SD card is available,
- picks the correct folder,
- deletes any older `data.csv` file before starting a new logging session.

### 6. How do you start and stop logging?

You control logging from the **web interface**:
- The ESP32 connects to Wi‑Fi (or creates its own hotspot, depending on build mode).
- It starts a small **web server**.
- On the web page, there are controls to **Start** and **Stop** logging.

When you press **Start**:
- the firmware:
  - creates / opens the CSV file,
  - writes the column header line (`timestamp_us,adc_value`),
  - resets the internal counters and timestamps,
  - starts putting samples into the queue and writing them to the CSV.

When you press **Stop**:
- the firmware:
  - stops sending new data into the queue,
  - waits a short time so any remaining samples in the queue are written,  
  - closes the file cleanly and updates its “last modified” time,  
  - records how long the logging lasted and how many samples were collected.

### 7. What is the logging rate and duration?

- **Target sampling rate**: about **4000 samples per second**.
- **Auto‑stop safety**: if the number of samples reaches a large limit (a built‑in safety cap), logging stops automatically so the storage does not fill forever.
- The firmware can compute:
  - how many samples were taken,
  - how long logging ran,
  - the actual average samples‑per‑second.

These values can be displayed in the web UI or logs to help you confirm that the experiment ran as expected.

#### Why don't I get exactly 1,200,000 samples after 300 seconds?

Even though the target rate is **4000 samples per second**, you might notice that after 300 seconds you get around **1,170,000 samples** instead of exactly **1,200,000** (which would be 300 × 4000). This is normal and happens for several reasons:

1. **Invalid samples are filtered out**: Sometimes the ADC hardware produces readings that are not valid (due to electrical noise or timing issues). The firmware automatically skips these bad readings to keep your data accurate.

2. **Queue can fill up**: The system uses a queue (a waiting line) to temporarily store samples before writing them to the SD card. If the SD card writes slower than samples arrive, the queue can fill up. When this happens, new samples are **dropped** (not saved) rather than slowing down the measurement rate. This ensures the timing stays consistent for the samples that **are** saved.

3. **Buffer overflows**: Occasionally, the ADC's internal buffer can overflow if the system is very busy. When this happens, a small number of samples may be lost.

4. **System overhead**: The ESP32 is doing many things at once (Wi‑Fi, web server, file writing). Small delays from these other tasks can cause tiny gaps in sampling.

5. **Actual rate may vary slightly**: The hardware might run at **3900 Hz** instead of exactly **4000 Hz** due to clock accuracy and system timing.

**This is expected behavior** — the firmware prioritizes **keeping accurate timing** for the samples that are saved, rather than forcing exactly 4000 samples per second. The actual rate (like ~3900 Hz in this example) is still very consistent and suitable for most experiments. You can check the actual rate by dividing your total sample count by the logging duration shown in the web interface.

### 8. What is the “bench” mode with CSV and BIN files?

For lab testing, there is a **bench mode**:
- It runs the laser and ADC for a **fixed amount of time** (for example 30 seconds),
- Writes to either:
  - a **CSV file** (`data.csv`) with text lines like above, or
  - a **binary file** (`data.bin`) with compact 12‑byte records (`timestamp_us` + `adc_value`).

The binary file is smaller and quicker to write.  
Later, a helper script (`tools/bin2csv.py`) on your computer can:
- read the binary file,
- convert it into a normal CSV with the same columns (`timestamp_us,adc_value`).

### 9. How do I use the CSV in a spreadsheet?

1. **Remove the SD card** from the device (or, if using SPIFFS, copy `/spiffs/data.csv` via a PC tool).
2. Insert the SD card into your computer.
3. Open `data.csv` in:
   - Excel,
   - Google Sheets,
   - LibreOffice Calc, or
   - any data analysis program that accepts CSV files.
4. You will see two columns:
   - `timestamp_us` (x‑axis, time),
   - `adc_value` (y‑axis, laser intensity).
5. Plot a graph with:
   - **X axis**: `timestamp_us` (you may want to divide by 1,000,000 to show seconds),
   - **Y axis**: `adc_value`.

This lets you see how the laser reflection changes over time, and you can compare different runs by loading multiple CSV files.

### 10. Summary in everyday words

- The board shines a **constant laser** onto your sample.
- A **light sensor** watches how bright the reflection is.
- The board takes **4,000 readings every second** and remembers each one as a number.
- Those numbers, together with precise **timestamps**, are saved into a **spreadsheet‑friendly CSV file**.
- You can then put the SD card into your computer and use Excel or a similar program to analyze or graph how the signal changes over time.

### 11. Additional Resources

For more review and analysis tools, refer to the **`tools`** folder outside this `main` directory. The tools folder contains scripts and utilities for processing and visualizing the CSV data files.

