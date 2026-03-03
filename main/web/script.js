const API_BASE = '';

function updateStatus(message) {
    const statusEl = document.getElementById('status');
    statusEl.textContent = message;
    statusEl.style.animation = 'none';
    setTimeout(() => {
        statusEl.style.animation = 'fade 0.5s';
    }, 10);
}

function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
}

let adcUpdateInterval = null;
let isLoggingActive = false;

function updateADC() {
    // Only update if logging is active
    if (!isLoggingActive) {
        return;
    }
    
    fetch(`${API_BASE}/api/adc`)
        .then(response => {
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}`);
            }
            return response.json();
        })
        .then(data => {
            // Update display when logging is active
            if (data.logging) {
                document.getElementById('adcValue').textContent = data.adc;
                const status = `Samples: ${data.samples}`;
                document.getElementById('adcInfo').textContent = status;
                // Show recording time in minutes (update every 30s is fine; we get it each poll)
                const elapsedEl = document.getElementById('elapsedTime');
                if (data.elapsed_ms != null) {
                    const totalMs = data.elapsed_ms;
                    const totalMin = Math.floor(totalMs / 60000);
                    const hours = Math.floor(totalMin / 60);
                    const mins = totalMin % 60;
                    elapsedEl.textContent = hours > 0
                        ? `Recording: ${hours}h ${mins} min`
                        : `Recording: ${totalMin} min`;
                    elapsedEl.style.display = '';
                } else {
                    elapsedEl.style.display = 'none';
                }
            } else {
                // When logging stops, stop updating display immediately
                isLoggingActive = false;
                if (adcUpdateInterval) {
                    clearInterval(adcUpdateInterval);
                    adcUpdateInterval = null;
                }
                document.getElementById('adcValue').textContent = '--';
                document.getElementById('adcInfo').textContent = 'Stopped';
                document.getElementById('elapsedTime').textContent = '';
                document.getElementById('elapsedTime').style.display = 'none';
            }
        })
        .catch(error => {
            // Silently handle connection errors - they're expected with frequent polling
            // Only log if it's not a network error
            if (error.name !== 'TypeError' && !error.message.includes('Failed to fetch')) {
                console.error('ADC fetch error:', error);
            }
        });
}

// Removed: downloadCSV() is now called automatically from stopLogging()
// Removed: clearStorage() function - feature disabled

function startLogging() {
    // If logging is already active, confirm with the user before restarting
    if (isLoggingActive) {
        const shouldReset = window.confirm('It already started, do you want to reset?');
        if (!shouldReset) {
            // User chose not to reset; just inform that recording is already running
            updateStatus('Recording is already running.');
            return;
        }
        // If user confirms reset, we simply call /api/start again.
        // The backend should handle resetting any previous recording state.
    }

    // Get client's current time and date
    const clientTime = new Date();
    const clientTimeISO = clientTime.toISOString();
    const clientTimestamp = clientTime.getTime(); // milliseconds since epoch
    const timezoneOffset = -clientTime.getTimezoneOffset(); // minutes offset from UTC

    fetch(`${API_BASE}/api/start`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            client_time: clientTimeISO,
            client_timestamp: clientTimestamp,
            timezone_offset: timezoneOffset
        })
    })
        .then(response => response.json())
        .then(data => {
            isLoggingActive = true;
            let statusMsg = `Recording started. Sample rate: ${data.rate_target ?? '?'} Hz`;
            updateStatus(statusMsg);
            document.getElementById('elapsedTime').textContent = 'Recording: 0 min';
            document.getElementById('elapsedTime').style.display = '';
            
            // Start updating ADC display
            if (!adcUpdateInterval) {
                updateADC(); // Initial update
                adcUpdateInterval = setInterval(updateADC, 100);
            }
        })
        .catch(error => {
            console.error('Start logging error:', error);
            updateStatus('Error starting recording');
        });
}

function downloadCSV() {
    // Create a temporary link to trigger download
    const link = document.createElement('a');
    link.href = `${API_BASE}/api/csv`;
    link.download = 'data.csv';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

function stopLogging() {
    // Stop ADC updates immediately (no confirm dialog)
    isLoggingActive = false;
    if (adcUpdateInterval) {
        clearInterval(adcUpdateInterval);
        adcUpdateInterval = null;
    }
    document.getElementById('adcValue').textContent = '--';
    document.getElementById('adcInfo').textContent = 'Stopped';
    
    fetch(`${API_BASE}/api/stop`, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            let statusMsg = `Recording stopped. Total samples: ${data.samples}`;
            if (data.rate_hz && data.rate_hz > 0) {
                statusMsg += ` | Rate: ${data.rate_hz.toFixed(2)} Hz`;
            }
            if (data.elapsed_ms) {
                const seconds = (data.elapsed_ms / 1000).toFixed(2);
                statusMsg += ` | Duration: ${seconds}s`;
            }
            updateStatus(statusMsg);
            
            // Auto-download CSV file if available
            if (data.csv_available) {
                // Small delay to ensure file is fully written
                setTimeout(() => {
                    const link = document.createElement('a');
                    link.href = `${API_BASE}/api/csv`;
                    link.download = 'data.csv';
                    document.body.appendChild(link);
                    link.click();
                    document.body.removeChild(link);
                }, 500);
            }
        })
        .catch(error => {
            console.error('Stop logging error:', error);
            updateStatus('Error stopping recording');
        });
}

// Auto-sync time with server when page loads
function syncTimeWithServer() {
    // Get client's current time and timezone
    const clientTime = new Date();
    const clientTimeISO = clientTime.toISOString();
    const clientTimestamp = clientTime.getTime(); // milliseconds since epoch
    const timezoneOffset = -clientTime.getTimezoneOffset(); // minutes offset from UTC (negative because getTimezoneOffset returns opposite)
    
    fetch(`${API_BASE}/api/sync_time`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            client_time: clientTimeISO,
            client_timestamp: clientTimestamp,
            timezone_offset: timezoneOffset
        })
    })
        .then(response => response.json())
        .then(data => {
            if (data.status === 'time_synced') {
                console.log('Time synced successfully:', data.client_time, 'Timezone offset:', data.timezone_offset, 'min');
            } else {
                console.warn('Time sync failed:', data.error || 'unknown error');
            }
        })
        .catch(error => {
            console.error('Time sync error:', error);
        });
}

// Start ADC updates on page load (but don't update until logging starts)
window.addEventListener('load', function() {
    // Don't start ADC updates automatically - wait for user to press start
    document.getElementById('adcValue').textContent = '--';
    document.getElementById('adcInfo').textContent = 'Press Start to begin';
    
    // Auto-sync time with server when page loads
    syncTimeWithServer();
});


// ===== Chunk mode: Polling API (replaces SSE for better stop responsiveness) =====

let chunkModeActive = false;
let chunkPollInterval = null;
let lastChunkIndex = -1;
let downloadedChunks = new Map(); // Map of chunk index -> CSV content

// Start chunk mode: use polling API to fetch chunks
function startChunkMode() {
    if (chunkModeActive) {
        updateStatus('Chunk mode already running.');
        return;
    }

    downloadedChunks.clear();
    lastChunkIndex = -1;
    chunkModeActive = true;

    // Get client's current time and date
    const clientTime = new Date();
    const clientTimeISO = clientTime.toISOString();
    const clientTimestamp = clientTime.getTime(); // milliseconds since epoch
    const timezoneOffset = -clientTime.getTimezoneOffset(); // minutes offset from UTC

    // Start chunked logging on ESP32
    fetch(`${API_BASE}/api/start_chunk`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            testbench: false,
            client_time: clientTimeISO,
            client_timestamp: clientTimestamp,
            timezone_offset: timezoneOffset
        })
    })
        .then(response => response.json())
        .then(data => {
            isLoggingActive = true;
            updateStatus(`Chunk mode: recording (${data.chunk_secs || 10}s/chunk, ${data.rate_hz || '?'} Hz)`);
            const chunkLine = document.getElementById('chunkModeLine');
            chunkLine.textContent = 'Chunk mode: recording...';
            chunkLine.style.display = '';
            document.getElementById('elapsedTime').style.display = '';
            if (!adcUpdateInterval) {
                updateADC();
                adcUpdateInterval = setInterval(updateADC, 100);
            }
            
            // Start polling for chunks (every 500ms)
            chunkPollInterval = setInterval(pollForChunks, 500);
        })
        .catch(err => {
            console.error('Start chunk error:', err);
            updateStatus('Error starting chunk mode');
            chunkModeActive = false;
        });
}

// Poll for new chunks and download them
async function pollForChunks() {
    if (!chunkModeActive) {
        return;
    }

    try {
        // Check chunk status
        const statusResponse = await fetch(`${API_BASE}/api/chunks`);
        if (!statusResponse.ok) {
            if (statusResponse.status === 404) {
                // Chunked logging not active - stop polling
                stopChunkPolling();
                return;
            }
            throw new Error(`HTTP ${statusResponse.status}`);
        }
        
        const status = await statusResponse.json();
        
        if (!status.active) {
            // Logging stopped - download all chunks
            stopChunkPolling();
            downloadAllChunks();
            return;
        }

        // Check for new chunks
        const currentChunkCount = status.chunks || 0;
        if (currentChunkCount > lastChunkIndex + 1) {
            // Download new chunks
            for (let i = lastChunkIndex + 1; i < currentChunkCount; i++) {
                await downloadChunk(i);
            }
            lastChunkIndex = currentChunkCount - 1;
            updateStatus(`Chunk mode: downloaded ${downloadedChunks.size} chunk(s)...`);
        }
        
        const chunkLine = document.getElementById('chunkModeLine');
        chunkLine.textContent = `Chunk mode: recording (${currentChunkCount} chunk(s) ready)`;
    } catch (error) {
        // Silently handle errors - they're expected with frequent polling
        if (error.name !== 'TypeError' && !error.message.includes('Failed to fetch')) {
            console.error('Chunk poll error:', error);
        }
    }
}

// Download a specific chunk (retries once if truncated)
async function downloadChunk(chunkIndex, retryCount = 0) {
    const maxRetries = 1;
    try {
        const response = await fetch(`${API_BASE}/api/chunk?index=${chunkIndex}`);
        if (!response.ok) {
            if (response.status === 404) {
                return;
            }
            throw new Error(`HTTP ${response.status}`);
        }
        
        const csvText = await response.text();
        const contentLength = response.headers.get('Content-Length');
        if (contentLength) {
            const expected = parseInt(contentLength, 10);
            if (!isNaN(expected) && csvText.length !== expected) {
                if (retryCount < maxRetries) {
                    console.warn(`Chunk ${chunkIndex}: truncated (got ${csvText.length}, expected ${expected}), retrying...`);
                    return downloadChunk(chunkIndex, retryCount + 1);
                }
                console.warn(`Chunk ${chunkIndex}: truncated (got ${csvText.length}, expected ${expected}), keeping partial`);
            }
        }
        
        if (csvText && csvText.trim()) {
            // Always strip header so stored content is data-only (avoids duplicate header in combined)
            const lines = csvText.trim().split('\n');
            if (lines.length > 0 &&
                lines[0].toLowerCase().includes('timestamp_us') &&
                lines[0].toLowerCase().includes('adc_value')) {
                lines.shift();
            }
            const processedText = lines.join('\n').trimEnd();
            if (processedText) {
                downloadedChunks.set(chunkIndex, processedText);
            }
        }
    } catch (error) {
        console.error(`Error downloading chunk ${chunkIndex}:`, error);
    }
}

// Stop chunk polling
function stopChunkPolling() {
    if (chunkPollInterval) {
        clearInterval(chunkPollInterval);
        chunkPollInterval = null;
    }
}

// Download all chunks as combined CSV
function downloadAllChunks() {
    if (downloadedChunks.size === 0) {
        updateStatus('Chunk mode stopped. No chunks received (threshold may not have been crossed).');
        return;
    }
    
    // Chunks are stored data-only (no header). Combine with single header.
    const sortedEntries = Array.from(downloadedChunks.entries()).sort((a, b) => a[0] - b[0]);
    const sortedChunks = sortedEntries.map(e => e[1]);
    const headerLine = 'timestamp_us,adc_value';
    const fullCsv = headerLine + '\n' + sortedChunks.join('\n');
    
    // Log per-chunk line counts to spot truncation (e.g. last chunk short)
    const lineCounts = sortedEntries.map(([idx, csv]) => {
        const n = csv.trim().split('\n').filter(l => l.length > 0).length;
        return `chunk ${idx}: ${n} lines`;
    });
    if (lineCounts.length) {
        console.info('Chunk line counts: ' + lineCounts.join(', '));
    }
    const totalRows = sortedChunks.join('\n').split('\n').filter(l => l.length > 0).length;
    updateStatus(`Downloaded ${downloadedChunks.size} chunk(s), ${totalRows.toLocaleString()} rows → data_chunks_combined.csv`);
    
    const blob = new Blob([fullCsv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = 'data_chunks_combined.csv';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
    downloadedChunks.clear();
}

// Stop chunk mode, wait for final chunks, assemble and download.
function stopChunkModeAndDownload() {
    if (!chunkModeActive) {
        updateStatus('Chunk mode is not active.');
        return;
    }

    isLoggingActive = false;
    if (adcUpdateInterval) {
        clearInterval(adcUpdateInterval);
        adcUpdateInterval = null;
    }
    document.getElementById('adcValue').textContent = '--';
    document.getElementById('adcInfo').textContent = 'Stopped';
    document.getElementById('chunkModeLine').style.display = 'none';
    document.getElementById('chunkModeLine').textContent = '';

    // Stop polling immediately
    stopChunkPolling();
    chunkModeActive = false;

    // Stop chunked logging on ESP32
    fetch(`${API_BASE}/api/stop_chunk`, { method: 'POST' })
        .then(response => response.json())
        .then(async () => {
            updateStatus('Chunk mode: stopped. Downloading final chunks...');
            // After stop, server returns 200 with { chunks: N, active: false }; fetch and download any missing chunks
            await new Promise(r => setTimeout(r, 800));
            try {
                const r = await fetch(`${API_BASE}/api/chunks`);
                if (r.ok) {
                    const status = await r.json();
                    const count = status.chunks || 0;
                    for (let i = 0; i < count; i++) {
                        if (!downloadedChunks.has(i)) await downloadChunk(i);
                    }
                }
            } catch (e) {
                console.warn('Final chunks fetch failed:', e);
            }
            downloadAllChunks();
        })
        .catch(err => {
            console.error('Stop chunk error:', err);
            downloadAllChunks();
        });
}


// ===== Download Test Bench (Chunked continuous capture) =====
//
// Conditions:  5 min, 30 min, 2 hours  (in the plastic box)
// Replicates:  n = 2
//
// Flow per trial:
//   1. POST /api/start_chunk {testbench:true}  (begin recording)
//   2. Poll /api/chunks + download each chunk during recording
//   3. After duration: POST /api/stop_chunk
//   4. Fetch remaining chunks, save individual + combined CSV

const TB_CONDITIONS = [
    { label: '5 min',   durationMs: 5  * 60 * 1000 },
    { label: '30 min',  durationMs: 30 * 60 * 1000 },
    { label: '2 hours', durationMs: 2  * 60 * 60 * 1000 },
];
const TB_REPLICATES = 2;

let tbAbortController = null;
let tbRunning = false;
let tbCountdownInterval = null;

function tbLog(html) {
    const el = document.getElementById('testBenchResults');
    el.innerHTML += html + '\n';
    el.scrollTop = el.scrollHeight;
}

function tbClear() {
    document.getElementById('testBenchResults').innerHTML = '';
}

function formatDuration(ms) {
    if (ms >= 3600000) {
        const h = Math.floor(ms / 3600000);
        const m = Math.floor((ms % 3600000) / 60000);
        const s = Math.floor((ms % 60000) / 1000);
        return `${h}h ${m}m ${s}s`;
    }
    if (ms >= 60000) {
        const m = Math.floor(ms / 60000);
        const s = Math.floor((ms % 60000) / 1000);
        return `${m}m ${s}s`;
    }
    return (ms / 1000).toFixed(2) + 's';
}

function tbSleep(ms, signal) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(resolve, ms);
        if (signal) {
            signal.addEventListener('abort', () => {
                clearTimeout(timer);
                reject(new DOMException('Aborted', 'AbortError'));
            });
        }
    });
}

function tbSaveBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
}

// Download a single chunk, stripping the CSV header so chunks can be combined.
// Returns { text, size } or null on failure.
async function tbDownloadChunk(index) {
    try {
        const resp = await fetch(`${API_BASE}/api/chunk?index=${index}`);
        if (!resp.ok) return null;
        const text = await resp.text();
        const size = text.length;
        // Strip header for combining later
        const lines = text.trim().split('\n');
        if (lines.length > 0 && lines[0].toLowerCase().includes('timestamp_us')) {
            lines.shift();
        }
        return { text: lines.join('\n').trimEnd(), size };
    } catch (e) {
        return null;
    }
}

async function generateTestBench() {
    if (tbRunning) {
        updateStatus('Test bench is already running.');
        return;
    }

    tbRunning = true;
    tbAbortController = new AbortController();
    const signal = tbAbortController.signal;
    document.getElementById('stopTestBenchBtn').disabled = false;

    tbClear();
    tbLog('<b>===== Download Test Bench (Chunked Continuous) =====</b>');
    tbLog(`Conditions: ${TB_CONDITIONS.map(c => c.label).join(', ')} | Replicates: n=${TB_REPLICATES}`);
    tbLog('Environment: in the plastic box');
    tbLog('---');

    const results = [];

    try {
        for (let ci = 0; ci < TB_CONDITIONS.length; ci++) {
            const cond = TB_CONDITIONS[ci];
            for (let rep = 1; rep <= TB_REPLICATES; rep++) {
                if (signal.aborted) throw new DOMException('Aborted', 'AbortError');

                const trialTag = `[${cond.label} | rep ${rep}/${TB_REPLICATES}]`;
                const safeLabel = cond.label.replace(/\s+/g, '_');
                tbLog(`\n<b>${trialTag}</b>`);

                // 1. Start chunk recording in testbench mode
                tbLog(`${trialTag} Starting chunked recording...`);
                const clientTime = new Date();
                const startResp = await fetch(`${API_BASE}/api/start_chunk`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        testbench: true,
                        client_time: clientTime.toISOString(),
                        client_timestamp: clientTime.getTime(),
                        timezone_offset: -clientTime.getTimezoneOffset()
                    }),
                    signal
                });
                const startData = await startResp.json();
                if (startData.status === 'error') {
                    tbLog(`${trialTag} <span style="color:#ff6b6b">Failed to start: ${startData.error}</span>`);
                    results.push({ condition: cond.label, replicate: rep, error: startData.error });
                    await tbSleep(2000, signal);
                    continue;
                }
                const chunkSecs = startData.chunk_secs || 10;
                tbLog(`${trialTag} Recording started (rate: ${startData.rate_hz ?? '?'} Hz, ${chunkSecs}s/chunk)`);
                updateStatus(`Test bench: ${trialTag} recording...`);

                // Enable live ADC display (value, samples, elapsed time)
                isLoggingActive = true;
                if (!adcUpdateInterval) {
                    updateADC();
                    adcUpdateInterval = setInterval(updateADC, 100);
                }

                // 2. Record for the condition duration while polling chunks
                const trialChunks = new Map(); // index -> { text, size }
                let lastChunkIdx = -1;
                let totalChunkBytes = 0;
                const trialStart = performance.now();
                const totalWaitMs = cond.durationMs;

                // Countdown + chunk polling combined
                const countdownEl = document.getElementById('testBenchResults');
                let countdownLine = document.createElement('span');
                countdownLine.id = 'tbCountdown';
                countdownEl.appendChild(countdownLine);

                const pollAndCountdown = async () => {
                    while (!signal.aborted) {
                        const elapsed = performance.now() - trialStart;
                        const remaining = Math.max(0, totalWaitMs - elapsed);

                        countdownLine.textContent = `${trialTag} Recording... ${formatDuration(Math.round(elapsed))} / ${formatDuration(totalWaitMs)} (${formatDuration(Math.round(remaining))} left) | chunks: ${trialChunks.size}`;
                        countdownEl.scrollTop = countdownEl.scrollHeight;

                        // Poll for new chunks (download to memory, save files at the end)
                        try {
                            const statusResp = await fetch(`${API_BASE}/api/chunks`);
                            if (statusResp.ok) {
                                const st = await statusResp.json();
                                const count = st.chunks || 0;
                                for (let i = lastChunkIdx + 1; i < count; i++) {
                                    const chunk = await tbDownloadChunk(i);
                                    if (chunk) {
                                        trialChunks.set(i, chunk);
                                        totalChunkBytes += chunk.size;
                                        lastChunkIdx = i;
                                        tbLog(`${trialTag} Chunk ${i} downloaded (${formatBytes(chunk.size)})`);
                                    }
                                }
                            }
                        } catch (_) {}

                        if (elapsed >= totalWaitMs) break;
                        await tbSleep(Math.min(1000, remaining), signal);
                    }
                };

                await pollAndCountdown();
                countdownLine.textContent = `${trialTag} Recording done: ${formatDuration(totalWaitMs)} | chunks: ${trialChunks.size}`;
                tbLog('');

                // 3. Stop recording (server waits for writer to finish)
                isLoggingActive = false;
                if (adcUpdateInterval) { clearInterval(adcUpdateInterval); adcUpdateInterval = null; }
                tbLog(`${trialTag} Stopping recording...`);
                const stopStart = performance.now();
                const stopResp = await fetch(`${API_BASE}/api/stop_chunk`, { method: 'POST', signal });
                const stopData = await stopResp.json().catch(() => ({}));
                if (stopData.confirmed) {
                    tbLog(`${trialTag} Stop confirmed by server`);
                } else {
                    tbLog(`${trialTag} <span style="color:#ffa502">Stop may not be fully confirmed, waiting...</span>`);
                    for (let w = 0; w < 10; w++) {
                        await tbSleep(500, signal);
                        const chk = await fetch(`${API_BASE}/api/chunks`).then(r => r.json()).catch(() => null);
                        if (chk && !chk.active) break;
                    }
                }

                // 4. Fetch remaining chunks after stop
                await tbSleep(500, signal);
                try {
                    const finalResp = await fetch(`${API_BASE}/api/chunks`);
                    if (finalResp.ok) {
                        const finalSt = await finalResp.json();
                        const finalCount = finalSt.chunks || 0;
                        for (let i = lastChunkIdx + 1; i < finalCount; i++) {
                            const chunk = await tbDownloadChunk(i);
                            if (chunk) {
                                trialChunks.set(i, chunk);
                                totalChunkBytes += chunk.size;
                                lastChunkIdx = i;
                                tbLog(`${trialTag} Chunk ${i} downloaded (${formatBytes(chunk.size)})`);
                            }
                        }
                    }
                } catch (_) {}

                const stopEnd = performance.now();
                const remainingDlMs = stopEnd - stopStart;

                // 5. Save individual chunk files + combined CSV
                tbLog(`${trialTag} Saving ${trialChunks.size} chunk files...`);
                const sortedEntries = Array.from(trialChunks.entries()).sort((a, b) => a[0] - b[0]);
                for (const [idx, chunk] of sortedEntries) {
                    tbSaveBlob(
                        new Blob([`timestamp_us,adc_value\n${chunk.text}`], { type: 'text/csv' }),
                        `testbench_${safeLabel}_rep${rep}_chunk${idx}.csv`
                    );
                    // Brief delay so browser doesn't block rapid downloads
                    await tbSleep(200, signal);
                }

                const sortedChunks = sortedEntries.map(e => e[1].text);
                const combinedCsv = 'timestamp_us,adc_value\n' + sortedChunks.join('\n');
                const combinedBlob = new Blob([combinedCsv], { type: 'text/csv' });
                const combinedFilename = `testbench_${safeLabel}_rep${rep}_combined.csv`;
                tbSaveBlob(combinedBlob, combinedFilename);

                tbLog(`${trialTag} <span style="color:#7bed9f">Complete</span> — ${trialChunks.size} chunks | Combined: <b>${formatBytes(combinedBlob.size)}</b> | After-stop download: <b>${formatDuration(remainingDlMs)}</b> → ${combinedFilename}`);

                results.push({
                    condition: cond.label,
                    replicate: rep,
                    fileSize: combinedBlob.size,
                    fileSizeStr: formatBytes(combinedBlob.size),
                    numChunks: trialChunks.size,
                    remainingDlMs: remainingDlMs,
                    remainingDlStr: formatDuration(remainingDlMs),
                });

                if (rep < TB_REPLICATES) {
                    tbLog(`${trialTag} Waiting 3s before next replicate...`);
                    await tbSleep(3000, signal);
                }
            }

            if (ci < TB_CONDITIONS.length - 1) {
                tbLog('\n--- Waiting 5s before next condition ---');
                await tbSleep(5000, signal);
            }
        }

        // Summary table
        tbLog('\n<b>===== RESULTS SUMMARY =====</b>');
        tbLog(padRow('Condition', 'Rep', 'Combined Size', 'Chunks', 'After-stop DL'));
        tbLog('─'.repeat(70));
        for (const r of results) {
            if (r.error) {
                tbLog(padRow(r.condition, r.replicate, 'ERROR', '-', r.error));
            } else {
                tbLog(padRow(r.condition, r.replicate, r.fileSizeStr, r.numChunks, r.remainingDlStr));
            }
        }
        tbLog('─'.repeat(70));
        tbLog('<b>Test bench complete.</b>');
        isLoggingActive = false;
        if (adcUpdateInterval) { clearInterval(adcUpdateInterval); adcUpdateInterval = null; }
        updateStatus('Test bench complete.');

    } catch (e) {
        if (e.name === 'AbortError') {
            isLoggingActive = false;
            if (adcUpdateInterval) { clearInterval(adcUpdateInterval); adcUpdateInterval = null; }
            if (tbCountdownInterval) { clearInterval(tbCountdownInterval); tbCountdownInterval = null; }
            tbLog('\n<span style="color:#ffa502"><b>Test bench stopped by user.</b></span>');
            if (results.length > 0) {
                tbLog('\n<b>===== RESULTS SO FAR =====</b>');
                tbLog(padRow('Condition', 'Rep', 'Combined Size', 'Chunks', 'After-stop DL'));
                tbLog('─'.repeat(70));
                for (const r of results) {
                    if (r.error) {
                        tbLog(padRow(r.condition, r.replicate, 'ERROR', '-', r.error));
                    } else {
                        tbLog(padRow(r.condition, r.replicate, r.fileSizeStr, r.numChunks, r.remainingDlStr));
                    }
                }
                tbLog('─'.repeat(70));
                tbLog(`<i>${results.length} trial(s) completed before stop.</i>`);
            }
            try { await fetch(`${API_BASE}/api/stop_chunk`, { method: 'POST' }); } catch (_) {}
            updateStatus('Test bench stopped. ' + (results.length > 0 ? results.length + ' trial(s) completed.' : ''));
        } else {
            tbLog(`\n<span style="color:#ff6b6b"><b>Error:</b> ${e.message}</span>`);
            updateStatus('Test bench error: ' + e.message);
        }
    } finally {
        tbRunning = false;
        tbAbortController = null;
        document.getElementById('stopTestBenchBtn').disabled = true;
    }
}

function padRow(cond, rep, size, chunks, dlTime) {
    return `${String(cond).padEnd(12)} ${String(rep).padEnd(5)} ${String(size).padEnd(16)} ${String(chunks).padEnd(10)} ${dlTime}`;
}

function stopTestBench() {
    if (tbAbortController) {
        tbAbortController.abort();
    }
}

