# Agent Notes — test branch

## Chunk Behavior (002.c)

### Why chunks have different sample counts
- Chunks are **time-based** (configured via `CHUNK_CONTINUOUS_SECS`), not fixed sample count.
- At 3000 Hz with 50s chunks, expect ~150,000 samples per chunk with small variation due to scheduling, queue contention, and SD card latency.

### Why chunk 0 is always smaller
- Chunk 0's timer starts **before** ADC is fully running (`start_sampling_timer()` is called after the timer is stamped).
- File open + header write + ADC spin-up consume part of chunk 0's time window.
- With 50s chunks the difference is negligible (~0.7%); with 10s chunks it was ~10%.

### Mid-recording stop behavior
- Writer immediately closes the current partial chunk (does not wait for the full time window).
- Partial chunk is saved, announced as ready, and downloadable.
- A sentinel (`index = -1`) signals "no more chunks" to the client.

---

## Adaptive Pause (applied in this branch)

### Previous behavior
- Fixed 1s `vTaskDelay` after every chunk close when `sample_rate >= 1000 Hz`.

### New behavior
- Pause is based on **queue fill level**, not sample rate.
- `< 50%` full: no pause.
- `50–75%` full: 500 ms pause.
- `> 75%` full: 1000 ms pause.
- At 3000 Hz the queue (48,000 slots = 16s buffer) stays nearly empty, so no pause triggers.

---

## Queue & Data Flow

### Pipeline
```
ADC task (CPU1, pri 5) → s_csv_queue (48,000 slots) → csv_writer_task (CPU0, pri 3) → chunk .csv on SD
```

### Key points
- ADC task **never stops** pushing to queue during chunk transitions or downloads.
- Writer drains queue **continuously** during a chunk — samples don't pile up for the full chunk duration.
- During close/open transition (~100–500 ms), ~300–1500 samples buffer in the queue; drained immediately when the next chunk opens.
- Client downloads a closed chunk file while the writer fills the next one (concurrent SD read + write).
- Client is always **one chunk behind** real-time.

### Queue overflow threshold
- Queue: 48,000 samples. At 3000 Hz = **16 seconds** of buffer.
- Overflow only if writer is **fully blocked for 16+ seconds** (e.g. SD card hang).
- Chunk duration does not affect overflow risk — the writer drains in real-time regardless.
- Risk increases at higher rates (e.g. 40 kHz → only 1.2s buffer).

---

## Test Plan (not yet implemented)

### On-device sanity checks
1. Confirm reported rate ~3000 Hz via `get_logging_stats`.
2. Run 5–10 chunks, confirm no "ADC buffer overflow" or "CSV queue nearly full" warnings.

### Host-side CSV tests (Python)
- **Structure**: first line is `timestamp_us,adc_value`, all rows parse as `(int64, int)`.
- **Sample count**: each chunk within ±10% of `rate * chunk_seconds`.
- **Monotonic timestamps**: strictly increasing within each chunk.
- **Cross-chunk continuity**: no large gaps or backwards jumps between consecutive chunks.

### Bench mode (optional)
- Verify `bench_laser_run_one` CSV/BIN sample count matches `rate * duration`.
- BIN file size = `n * 12 bytes` (packed `bench_record_t`).
