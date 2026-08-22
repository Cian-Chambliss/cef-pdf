'use strict';

const form = document.querySelector('#convertForm');
const format = document.querySelector('#format');
const pdfControls = document.querySelector('#pdfControls');
const imageControls = document.querySelector('#imageControls');
const statusText = document.querySelector('#conversionStatus');
const submitButton = document.querySelector('#submitButton');
const downloadLink = document.querySelector('#downloadLink');
const workerState = document.querySelector('#workerState');
const workerDetail = document.querySelector('#workerDetail');
const stateDot = document.querySelector('#stateDot');
const idleTimeout = document.querySelector('#idleTimeout');
const saveSettings = document.querySelector('#saveSettings');
let csrfToken = '';
let csrfRequest = null;

function setFormatControls() {
  const pdf = format.value === 'pdf';
  pdfControls.hidden = !pdf;
  imageControls.hidden = pdf;
}

function formBody() {
  const data = new FormData(form);
  const body = Object.fromEntries(data.entries());
  for (const name of ['waitSignal', 'saveHtml', 'staticOnly', 'landscape', 'backgrounds', 'headerFooter']) {
    body[name] = data.has(name);
  }
  return body;
}

async function loadCsrfToken() {
  if (csrfToken) return csrfToken;
  if (!csrfRequest) {
    csrfRequest = fetch('/api/status', { credentials: 'same-origin' })
      .then(async (response) => {
        const body = await response.json();
        if (!response.ok) throw new Error(body.error?.message || `Request failed with HTTP ${response.status}`);
        csrfToken = body.csrfToken;
        return csrfToken;
      })
      .finally(() => { csrfRequest = null; });
  }
  return csrfRequest;
}

async function jsonRequest(url, options = {}) {
  const requestOptions = { ...options, credentials: 'same-origin' };
  if (String(requestOptions.method || 'GET').toUpperCase() === 'POST') {
    await loadCsrfToken();
    requestOptions.headers = { ...requestOptions.headers, 'X-CSRF-Token': csrfToken };
  }
  const response = await fetch(url, requestOptions);
  const body = await response.json();
  if (!response.ok) throw new Error(body.error?.message || `Request failed with HTTP ${response.status}`);
  if (body.csrfToken) csrfToken = body.csrfToken;
  return body;
}

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  submitButton.disabled = true;
  downloadLink.hidden = true;
  statusText.textContent = 'Rendering...';
  try {
    const result = await jsonRequest('/api/convert', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(formBody())
    });
    downloadLink.href = result.downloadUrl;
    downloadLink.download = result.fileName;
    downloadLink.textContent = `Download ${result.fileName} once`;
    downloadLink.hidden = false;
    statusText.textContent = result.snapshotSaved ? 'Complete. A DOM snapshot was also saved temporarily.' : 'Complete.';
  } catch (error) {
    statusText.textContent = `Render failed: ${error.message}`;
  } finally {
    submitButton.disabled = false;
    refreshStatus();
  }
});

downloadLink.addEventListener('click', () => {
  statusText.textContent = 'Download started. This link is now spent.';
  setTimeout(() => { downloadLink.hidden = true; }, 0);
});

async function refreshStatus() {
  try {
    const status = await jsonRequest('/api/status');
    workerState.textContent = status.state;
    if (status.pid) workerDetail.textContent = `PID ${status.pid}, generation ${status.generation}, ${status.activeRequests} active`;
    else if (status.error) workerDetail.textContent = status.error;
    else if (status.state === 'stopped after inactivity') workerDetail.textContent = `generation ${status.generation}, next conversion starts a fresh worker`;
    else workerDetail.textContent = `generation ${status.generation}, next conversion starts the worker`;
    stateDot.dataset.state = status.state;
    if (document.activeElement !== idleTimeout) idleTimeout.value = status.idleTimeout;
  } catch (error) {
    workerState.textContent = 'status unavailable';
    workerDetail.textContent = error.message;
    stateDot.dataset.state = 'failed';
  }
}

saveSettings.addEventListener('click', async () => {
  saveSettings.disabled = true;
  try {
    const result = await jsonRequest('/api/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ idleTimeout: Number(idleTimeout.value) })
    });
    idleTimeout.value = result.idleTimeout;
    statusText.textContent = 'Idle timeout updated.';
  } catch (error) {
    statusText.textContent = `Settings failed: ${error.message}`;
  } finally {
    saveSettings.disabled = false;
  }
});

format.addEventListener('change', setFormatControls);
setFormatControls();
refreshStatus();
setInterval(refreshStatus, 1500);
