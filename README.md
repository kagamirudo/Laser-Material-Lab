# Laser Material Lab - ESP32-S3 Data Logger

A high-speed ADC data acquisition system for laser/material interaction studies using ESP32-S3. Features continuous ADC sampling, CSV file logging to SPIFFS, and a web-based control interface.

## Hardware Setup

### Wiring

- **Laser module**
  - **VCC** → ESP32-S3 **3V3**
  - **GND** → ESP32-S3 **GND**
  - **Signal (laser enable)** → **GPIO5**

- **Light sensor / receiver (photodiode / phototransistor module)**
  - **VCC** → ESP32-S3 **3V3**
  - **GND** → ESP32-S3 **GND**
  - **Analog output** → **GPIO4** (ADC1_CH3 on ESP32-S3)

> **Note:** GPIO44 is *not* ADC-capable on ESP32-S3; use GPIO4 (or another ADC pin) for the analog signal.

## Features

- **High-speed continuous ADC sampling**: Configurable sample rate (minimum 611 Hz, hardware-dependent rounding)
- **CSV file logging**: Automatic logging to SPIFFS (1MB partition) with auto-download on stop
- **Web-based control interface**: HTTPS web server for remote monitoring and control
- **Dual-core RTOS architecture**: 
  - Core 0: CSV writer task, HTTP server, Wi-Fi
  - Core 1: ADC sampling task
- **Auto-stop functionality**: Automatically stops logging after reaching sample limit
- **Dynamic filenames**: CSV files named with actual vs attempted sample rate (e.g., `data_3000Hz_attempt_3200.csv`)
- **Real-time monitoring**: Live ADC values displayed on web UI and serial terminal

## Configuration

### Sample Rate

Edit `main/002.c` to configure the target sample rate:

```c
#define SAMPLE_RATE_HZ    3200    // Target sample rate (Hz)
#define ADC_MIN_FREQ_HZ   611     // Minimum supported frequency
```

**Important:** The ESP32-S3 continuous ADC hardware only supports specific frequencies. If you request a frequency that's not supported, the driver will round to the nearest supported value. The actual rate used will be shown in the CSV filename.

### Sample Limit

Configure auto-stop limit:

```c
#define SAMPLE_LIMIT 10000    // Auto-stop after this many samples
```

### Wi-Fi Mode

Configure Wi-Fi or Access Point mode in `main/002.c`:

```c
#define MODE WIFI   // Connect to existing Wi-Fi network
// or
#define MODE HOST   // Create access point (default)
```

## Building and Flashing

### Prerequisites

- ESP-IDF v5.0 or later
- Python 3.8+

### Quick Start

```bash
# Build, flash, and monitor
make

# Or step by step:
make build      # Build the project
make flash      # Flash to device
make monitor    # Monitor serial output
```

### Makefile Targets

```bash
make              # Build, flash, and monitor (default)
make build        # Build only
make flash        # Flash only
make monitor      # Monitor serial output
make bfm          # Build, flash, and monitor
make bf           # Build and flash (no monitor)
make clean        # Clean build artifacts
make menuconfig   # Open configuration menu
make size         # Show binary size
make push         # Commit and push to git (default message)
make push MSG="your message"  # Commit and push with custom message
```

## Usage

### 1. Initial Setup

1. Wire the hardware as described above
2. Configure Wi-Fi settings if using `MODE WIFI`
3. Build and flash the firmware

### 2. Starting Data Collection

**Via Web Interface:**
1. Connect to the ESP32-S3 Wi-Fi network (or connect to the same network if using WIFI mode)
2. Open a browser and navigate to `https://192.168.4.1` (or the assigned IP)
3. Click "Start Recording" to begin data collection
4. Click "Stop Recording" to stop and automatically download the CSV file

**Via Serial Terminal:**
- Monitor the serial output to see real-time ADC values and status messages

### 3. CSV File Format

The CSV file contains two columns:
- `timestamp_us`: Timestamp in microseconds (starts at 0, consistent interval based on sample rate)
- `adc_value`: 12-bit ADC reading (0-4095, representing 0V to ~3.3V)

**Example:**
```csv
timestamp_us,adc_value
0,1234
250,1235
500,1236
750,1237
```

**ADC Value Interpretation:**
- Range: 0 to 4095 (12-bit ADC)
- Voltage: `voltage = (adc_value / 4095.0) * 3.3V`
- Example: `adc_value = 2048` ≈ 1.65V

### 4. Filename Convention

CSV files are automatically named based on the actual and attempted sample rates:
- `data_3000Hz_attempt_3200.csv` - Requested 3200 Hz, got 3000 Hz (hardware rounding)
- `data_1600Hz_attempt_1600.csv` - Requested 1600 Hz, got exactly 1600 Hz
- `data_611Hz_attempt_400.csv` - Requested 400 Hz, clamped to minimum 611 Hz

## Architecture

### RTOS Tasks

- **`adc_task`** (Core 1, Priority 5)
  - Reads samples from continuous ADC buffer
  - Enqueues samples to CSV queue (non-blocking)
  - Updates current ADC value for web UI
  - Handles auto-stop when sample limit reached

- **`csv_writer_task`** (Core 0, Priority 3)
  - Dequeues samples from queue
  - Writes batches to CSV file on SPIFFS
  - Calculates consistent timestamps based on sample index
  - Handles file flushing and closing

- **HTTP Server** (Core 0)
  - Serves web UI and API endpoints
  - Provides real-time ADC values via JSON API
  - Handles CSV file downloads

### Data Flow

```
ADC Hardware → adc_task → Queue → csv_writer_task → SPIFFS (CSV file)
                      ↓
                  Web UI (real-time display)
```

### Storage

- **SPIFFS Partition**: 1MB for CSV file storage
- **Auto-cleanup**: Old CSV files are deleted when starting new logging session
- **Auto-format**: SPIFFS is automatically reformatted if full

## Troubleshooting

### "ADC sampling frequency out of range" Error

The requested sample rate is below the minimum (611 Hz). The code will automatically clamp to the minimum, but you'll see a warning. Use a rate ≥ 611 Hz.

### "Failed to open CSV file (errno: 28)"

SPIFFS is full. The system will attempt to auto-format, but you may need to manually clear files or increase the SPIFFS partition size.

### Sample Rate Doesn't Match Requested

The ESP32-S3 ADC hardware only supports specific frequencies. The driver rounds to the nearest supported value. Check the CSV filename to see the actual rate used.

### Lost Samples

If you notice missing samples at the end of a recording:
- The system waits for the queue to drain before closing the file
- Ensure sufficient SPIFFS space is available
- Check that the sample limit wasn't reached mid-batch

## Development

### Project Structure

```
.
├── main/
│   ├── 002.c          # Main application code (ADC, CSV logging, RTOS tasks)
│   ├── server.c       # HTTP server and API endpoints
│   ├── wifi.c         # Wi-Fi configuration
│   ├── web/           # Web UI files (HTML, CSS, JS)
│   └── certs/         # HTTPS certificates
├── partitions.csv     # Partition table (SPIFFS configuration)
├── Makefile          # Build shortcuts
└── CMakeLists.txt    # ESP-IDF build configuration
```

### Key Constants

- `SAMPLE_RATE_HZ`: Target sampling frequency
- `SAMPLE_LIMIT`: Auto-stop sample count
- `CSV_QUEUE_SIZE`: Queue buffer size (3000 samples)
- `CSV_WRITE_BATCH_SIZE`: Batch size for CSV writes (500 samples)

## License

[Add your license here]

## Acknowledgments

Originally adapted from an Arduino UNO sketch for laser/material interaction studies.
