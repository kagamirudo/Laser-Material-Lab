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

**Code snippet** - ADC configuration:

```c
// ADC sampling config
#define SAMPLE_RATE_HZ    4000           // Target: 4000 samples per second
#define ADC_GPIO          4              // ADC input from photodiode/sensor

// Configure continuous ADC
adc_continuous_config_t cont_cfg = {
    .pattern_num = 1,
    .adc_pattern = &adc_pattern,
    .sample_freq_hz = s_actual_sample_rate_hz,  // 4000 Hz
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
};
ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cont_cfg));
```


### 3. What happens to each measurement?

For each raw measurement coming from the sensor:
- The ADC produces a number (the **ADC value**) that represents how bright the laser reflection is at that moment.
- The firmware keeps track of how many samples have been taken.
- When logging is enabled, each sample is:
  - packaged with a **timestamp** (time in microseconds since logging started), and
  - placed into a **queue** (a first‑in/first‑out buffer) so writing to storage does not slow down the measurements.

If the queue ever fills up, a few samples may be dropped. The firmware prefers to **keep sampling fast** rather than pause and risk timing gaps.

**Code snippet** - ADC reading and queue processing:

```c
// Structure for queued samples
typedef struct {
    int adc_value;
    uint64_t timestamp_us;
} csv_sample_t;

// In ADC task: read samples and enqueue them
esp_err_t parse_ret = adc_continuous_read_parse(adc_handle, parsed_samples, 256, 
                                                 &num_samples, ADC_READ_TIMEOUT_MS);

if (parse_ret == ESP_OK && num_samples > 0) {
    for (uint32_t i = 0; i < num_samples; i++) {
        if (!parsed_samples[i].valid) {
            dropped_samples++;  // Skip invalid samples
            continue;
        }
        
        int raw_value = (int)parsed_samples[i].raw_data;
        s_sample_count++;  // Track total samples
        
        // Send to CSV queue if logging enabled (non-blocking)
        if (s_csv_logging_enabled && s_csv_queue != NULL) {
            csv_sample_t sample = {
                .adc_value = raw_value,
                .timestamp_us = esp_timer_get_time()
            };
            // Non-blocking send - drops sample if queue is full
            if (xQueueSend(s_csv_queue, &sample, 0) != pdTRUE) {
                // Queue full - sample dropped to maintain ADC rate
            }
        }
    }
}
```

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

**Code snippet** - CSV writer task (batch writing):

```c
#define CSV_WRITE_BATCH_SIZE 500    // Write CSV in batches
#define CSV_FLUSH_INTERVAL 5        // Flush every N batches

static void csv_writer_task(void *pvParameters) {
    csv_sample_t batch[CSV_WRITE_BATCH_SIZE];
    uint32_t batch_count = 0;
    const uint64_t sample_interval_us = 1000000ULL / s_actual_sample_rate_hz;
    
    while (1) {
        // Wait for samples from queue
        csv_sample_t sample;
        if (xQueueReceive(s_csv_queue, &sample, pdMS_TO_TICKS(100)) == pdTRUE) {
            batch[batch_count] = sample;
            batch_count++;
            
            // Write batch when full
            if (batch_count >= CSV_WRITE_BATCH_SIZE) {
                // Write batch to CSV file
                for (uint32_t i = 0; i < batch_count; i++) {
                    uint64_t timestamp_us = s_csv_sample_index * sample_interval_us;
                    fprintf(s_csv_file, "%llu,%d\n",
                           (unsigned long long)timestamp_us,
                           batch[i].adc_value);
                    s_csv_sample_index++;
                }
                // Flush periodically to reduce flash wear
                static uint32_t flush_counter = 0;
                if (++flush_counter >= CSV_FLUSH_INTERVAL) {
                    fflush(s_csv_file);
                    flush_counter = 0;
                }
                batch_count = 0;
            }
        }
    }
}
```

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

**Code snippet** - Storage location selection:

```c
#define CSV_SD_DIR          SDCARD_MOUNT_POINT "/laser"
#define SPIFFS_MOUNT_POINT  "/spiffs"

static void generate_csv_filename(void) {
    const char *base_dir;
    if (sdcard_is_mounted()) {
        base_dir = CSV_SD_DIR;  // Use SD card: /sdcard/laser/data.csv
    } else {
        base_dir = SPIFFS_MOUNT_POINT;  // Use SPIFFS: /spiffs/data.csv
    }
    snprintf(s_csv_file_path, sizeof(s_csv_file_path), "%s/data.csv", base_dir);
    ESP_LOGI(TAG, "CSV path: %s", s_csv_file_path);
}
```

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

**Code snippet** - Start logging:

```c
void start_csv_logging(void) {
    generate_csv_filename();  // Choose SD card or SPIFFS
    
    // Open CSV file and write header
    s_csv_file = fopen(s_csv_file_path, "w");
    if (s_csv_file != NULL) {
        fprintf(s_csv_file, "timestamp_us,adc_value\n");
        fflush(s_csv_file);
    }
    
    // Reset counters and enable logging
    s_logging_start_time_us = esp_timer_get_time();
    s_sample_count = 0;
    s_csv_sample_index = 0;
    s_csv_logging_enabled = true;
    start_sampling_timer();  // Start ADC sampling
}
```

**Code snippet** - Stop logging:

```c
void stop_csv_logging(void) {
    s_csv_logging_enabled = false;  // Stop sending samples to queue
    
    // Wait for queue to drain
    while (uxQueueMessagesWaiting(s_csv_queue) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Close file and update modification time
    if (s_csv_file != NULL) {
        fflush(s_csv_file);
        fclose(s_csv_file);
        set_file_mtime_now(s_csv_file_path);
    }
    
    s_logging_stop_time_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Logging stopped. Total samples: %d", s_sample_count);
}
```

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

**Code snippet** - Bench mode binary record format:

```c
// Binary bench record (12 bytes: uint64 timestamp + int32 adc_value)
typedef struct {
    uint64_t timestamp_us;
    int32_t  adc_value;
} __attribute__((packed)) bench_record_t;

// Write binary record
bench_record_t rec = {
    .timestamp_us = s_bench_buffer[i].timestamp_us,
    .adc_value    = (int32_t)s_bench_buffer[i].adc_value,
};
fwrite(&rec, 1, sizeof(rec), f);  // Write 12-byte record

// Or write CSV format
fprintf(f, "%llu,%d\n",
        (unsigned long long)s_bench_buffer[i].timestamp_us,
        s_bench_buffer[i].adc_value);
```

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

