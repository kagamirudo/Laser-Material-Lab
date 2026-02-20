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
            let statusMsg = `Recording started. Target rate: ${data.rate_target || 4000} Hz`;
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
            client_time: clientTimeISO,
            client_timestamp: clientTimestamp,
            timezone_offset: timezoneOffset
        })
    })
        .then(response => response.json())
        .then(data => {
            isLoggingActive = true;
            updateStatus(`Chunk mode: waiting for ADC threshold, then ${data.peak || 10} cycles...`);
            // Chunk mode on its own line; recording time stays on elapsedTime (from updateADC)
            const chunkLine = document.getElementById('chunkModeLine');
            chunkLine.textContent = 'Chunk mode: polling for chunks...';
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
        
        // Update chunk mode line only (recording time stays on elapsedTime from updateADC)
        const chunkLine = document.getElementById('chunkModeLine');
        if (status.triggered) {
            // current_cycle is 0-based (0..PEAK-1); show 1-based (1..PEAK)
            const cycleDisplay = (status.current_cycle != null) ? (status.current_cycle + 1) : '?';
            chunkLine.textContent = 
                `Chunk mode: active (chunk ${currentChunkCount}, cycle ${cycleDisplay})`;
        } else {
            chunkLine.textContent = 'Chunk mode: waiting for threshold...';
        }
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
        .then(data => {
            updateStatus('Chunk mode: stopped. Downloading final chunks...');
            // Poll one more time to get any final chunks, then download
            setTimeout(async () => {
                await pollForChunks();
                downloadAllChunks();
            }, 1000);
        })
        .catch(err => {
            console.error('Stop chunk error:', err);
            // Still try to download what we have
            downloadAllChunks();
        });
}

