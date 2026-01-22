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
            } else {
                // When logging stops, stop updating display immediately
                isLoggingActive = false;
                if (adcUpdateInterval) {
                    clearInterval(adcUpdateInterval);
                    adcUpdateInterval = null;
                }
                document.getElementById('adcValue').textContent = '--';
                document.getElementById('adcInfo').textContent = 'Stopped';
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
    fetch(`${API_BASE}/api/start`, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            isLoggingActive = true;
            let statusMsg = `Recording started. Target rate: ${data.rate_target || 4000} Hz`;
            updateStatus(statusMsg);
            
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

