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

function updateADC() {
    fetch(`${API_BASE}/api/adc`)
        .then(response => {
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}`);
            }
            return response.json();
        })
        .then(data => {
            document.getElementById('adcValue').textContent = data.adc;
            const status = data.logging ? `Samples: ${data.samples}/100000` : `Stopped: ${data.samples} samples`;
            document.getElementById('adcInfo').textContent = status;
            
            // Update storage info
            if (data.storage) {
                const storageEl = document.getElementById('storageInfo');
                if (storageEl) {
                    const used = data.storage.used || 0;
                    const free = data.storage.free || 0;
                    const total = data.storage.total || 0;
                    if (total > 0) {
                        const usedPercent = Math.round((used / total) * 100);
                        storageEl.textContent = `Storage: ${formatBytes(free)} free / ${formatBytes(total)} total (${usedPercent}% used)`;
                    } else {
                        storageEl.textContent = 'Storage: Not available';
                    }
                }
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
            updateStatus(`Recording started. Samples: ${data.samples}`);
            updateADC(); // Refresh display
        })
        .catch(error => {
            console.error('Start logging error:', error);
            updateStatus('Error starting recording');
        });
}

function stopLogging() {
    if (!confirm('Stop recording data points? The CSV file will be automatically downloaded.')) {
        return;
    }
    
    fetch(`${API_BASE}/api/stop`, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            updateStatus(`Recording stopped. Total samples: ${data.samples}. Downloading CSV...`);
            updateADC(); // Refresh display
            
            // Automatically download CSV file after stopping
            downloadCSV();
        })
        .catch(error => {
            console.error('Stop logging error:', error);
            updateStatus('Error stopping recording');
        });
}

// Internal function to download CSV (called automatically after stop)
function downloadCSV() {
    // Use fetch to handle errors properly
    fetch(`${API_BASE}/api/csv`)
        .then(response => {
            // Check if response is JSON (error) or CSV (success)
            const contentType = response.headers.get('content-type');
            if (contentType && contentType.includes('application/json')) {
                // It's an error response
                return response.json().then(data => {
                    throw new Error(data.error || 'No CSV file available');
                });
            }
            // It's a CSV file, proceed with download
            if (!response.ok) {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            return response.blob();
        })
        .then(blob => {
            // Create download link and trigger download
            const url = window.URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = 'adc_data.csv';
            document.body.appendChild(a);
            a.click();
            window.URL.revokeObjectURL(url);
            document.body.removeChild(a);
            updateStatus('CSV file downloaded successfully');
        })
        .catch(error => {
            console.error('CSV download error:', error);
            updateStatus(error.message || 'No CSV file available. Please start recording first.');
        });
}

// Removed: clearStorage() function - feature disabled

// Start ADC updates on page load
window.addEventListener('load', function() {
    updateADC(); // Initial update
    // Update every 100ms - balanced between responsiveness and server load
    // This gives 10 updates/second which is smooth enough for real-time monitoring
    setInterval(updateADC, 100);
});

