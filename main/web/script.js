const API_BASE = '';

function updateStatus(message) {
    const statusEl = document.getElementById('status');
    statusEl.textContent = message;
    statusEl.style.animation = 'none';
    setTimeout(() => {
        statusEl.style.animation = 'fade 0.5s';
    }, 10);
}

function setColor(r, g, b) {
    fetch(`${API_BASE}/api/color?r=${r}&g=${g}&b=${b}`, {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        updateStatus(`Color set: RGB(${r}, ${g}, ${b})`);
    })
    .catch(error => {
        updateStatus('Error: ' + error);
        console.error('Error:', error);
    });
}

function setCustomColor() {
    const colorPicker = document.getElementById('colorPicker');
    const brightness = document.getElementById('brightness').value;
    
    const hex = colorPicker.value;
    const r = parseInt(hex.substr(1, 2), 16);
    const g = parseInt(hex.substr(3, 2), 16);
    const b = parseInt(hex.substr(5, 2), 16);
    
    // Apply brightness
    const rAdj = Math.floor(r * brightness / 255);
    const gAdj = Math.floor(g * brightness / 255);
    const bAdj = Math.floor(b * brightness / 255);
    
    setColor(rAdj, gAdj, bAdj);
}

function startEffect(effect) {
    fetch(`${API_BASE}/api/effect?name=${effect}`, {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        updateStatus(`Effect started: ${effect}`);
    })
    .catch(error => {
        updateStatus('Error: ' + error);
        console.error('Error:', error);
    });
}

function stopEffect() {
    fetch(`${API_BASE}/api/stop`, {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        updateStatus('Effect stopped');
    })
    .catch(error => {
        updateStatus('Error: ' + error);
        console.error('Error:', error);
    });
}

function turnOff() {
    fetch(`${API_BASE}/api/off`, {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        updateStatus('LED turned off');
    })
    .catch(error => {
        updateStatus('Error: ' + error);
        console.error('Error:', error);
    });
}

// Update brightness display
document.getElementById('brightness').addEventListener('input', function(e) {
    document.getElementById('brightnessValue').textContent = e.target.value;
});

// ADC reading update
let adcUpdateInterval = null;

function updateADC() {
    fetch(`${API_BASE}/api/adc`)
        .then(response => response.json())
        .then(data => {
            document.getElementById('adcValue').textContent = data.adc;
            const status = data.logging ? `Samples: ${data.samples}/10000` : `Stopped: ${data.samples} samples`;
            document.getElementById('adcInfo').textContent = status;
        })
        .catch(error => {
            console.error('ADC fetch error:', error);
        });
}

function downloadCSV() {
    window.location.href = `${API_BASE}/api/csv`;
}

// Start ADC updates on page load
window.addEventListener('load', function() {
    updateADC(); // Initial update
    adcUpdateInterval = setInterval(updateADC, 500); // Update every 500ms
});

