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

    fetch(`${API_BASE}/api/start`, { method: 'POST' })
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

// Start ADC updates on page load (but don't update until logging starts)
window.addEventListener('load', function() {
    // Don't start ADC updates automatically - wait for user to press start
    document.getElementById('adcValue').textContent = '--';
    document.getElementById('adcInfo').textContent = 'Press Start to begin';
});


// ===== Chunk mode: SSE (Server-Sent Events) for low-latency chunk delivery =====

let chunkModeActive = false;
let csvChunks = [];
let chunkEventSource = null;
// Start chunk mode: connect to SSE stream first, then start chunked logging.
function startChunkMode() {
    if (chunkModeActive) {
        updateStatus('Chunk mode already running.');
        return;
    }

    csvChunks = [];
    chunkModeActive = true;

    // 1. Connect to SSE stream (must be before start)
    const streamUrl = `${API_BASE}/api/csv_stream`.replace(/^\/+/, '/');
    chunkEventSource = new EventSource(streamUrl);

    chunkEventSource.onmessage = function(e) {
        if (e.data === 'done') {
            chunkEventSource.close();
            chunkEventSource = null;
            chunkModeActive = false;
            if (csvChunks.length > 0) {
                const fullCsv = csvChunks.join('\n');
                const blob = new Blob([fullCsv], { type: 'text/csv' });
                const url = URL.createObjectURL(blob);
                const link = document.createElement('a');
                link.href = url;
                link.download = 'data_chunks_combined.csv';
                document.body.appendChild(link);
                link.click();
                document.body.removeChild(link);
                URL.revokeObjectURL(url);
                updateStatus(`Downloaded ${csvChunks.length} chunk(s) as data_chunks_combined.csv`);
            } else {
                updateStatus('Chunk mode stopped. No chunks received (threshold may not have been crossed).');
            }
            return;
        }
        let text = e.data;
        if (!text || !text.trim()) return;
        if (csvChunks.length > 0) {
            const lines = text.split('\n');
            if (lines.length > 0 &&
                lines[0].toLowerCase().includes('timestamp_us') &&
                lines[0].toLowerCase().includes('adc_value')) {
                lines.shift();
            }
            text = lines.join('\n').trimEnd();
        }
        if (text) csvChunks.push(text.trimEnd());
        updateStatus(`Chunk mode: received ${csvChunks.length} chunk(s)...`);
    };

    chunkEventSource.onerror = function() {
        if (chunkModeActive && chunkEventSource) {
            console.warn('SSE connection error - may reconnect');
        }
    };

    // 2. Start chunked logging on ESP32
    fetch(`${API_BASE}/api/start_chunk`, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            isLoggingActive = true;
            updateStatus(`Chunk mode: waiting for ADC threshold, then ${data.peak || 10} cycles...`);
            document.getElementById('elapsedTime').textContent = 'Chunk mode: streaming...';
            document.getElementById('elapsedTime').style.display = '';
            if (!adcUpdateInterval) {
                updateADC();
                adcUpdateInterval = setInterval(updateADC, 100);
            }
        })
        .catch(err => {
            console.error('Start chunk error:', err);
            updateStatus('Error starting chunk mode');
            chunkModeActive = false;
            if (chunkEventSource) {
                chunkEventSource.close();
                chunkEventSource = null;
            }
        });
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

    fetch(`${API_BASE}/api/stop_chunk`, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            updateStatus('Chunk mode: stopping... (finishing current cycle, then assembling)');
        })
        .catch(err => {
            console.error('Stop chunk error:', err);
        });

    // Download is triggered in onmessage when SSE sends "done"
}

