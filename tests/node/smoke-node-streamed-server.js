#!/usr/bin/env node
'use strict';

const fs = require('fs');
const http = require('http');
const path = require('path');
const { spawn } = require('child_process');

const root = path.resolve(__dirname, '..', '..');
const serverScript = path.join(root, 'examples', 'node-streamed-server', 'server.js');

if (process.argv[2] === '--server-child') {
  process.argv.splice(2, 1);
  process.on('message', message => {
    if (message && message.command === 'shutdown') process.emit('SIGTERM');
  });
  require(serverScript);
} else {
  run().catch(error => {
    console.error(`node server lifecycle smoke failed: ${error.message}`);
    process.exitCode = 1;
  });
}

function fail(message) {
  throw new Error(message);
}

function resolveExecutable(requested) {
  if (requested) {
    const resolved = path.resolve(requested);
    if (!fs.existsSync(resolved)) fail(`cef-pdf executable not found: ${resolved}`);
    return resolved;
  }
  const candidates = process.platform === 'win32' ? [
    'build/vs17-x64-7680/src/Release/cef-pdf.exe',
    'build/src/Release/cef-pdf.exe',
    'src/Release/cef-pdf.exe',
    'build/Release/cef-pdf.exe',
    'Release/cef-pdf.exe',
    'build/src/cef-pdf.exe'
  ] : [
    'build/src/cef-pdf',
    'build/cef-pdf',
    'src/cef-pdf'
  ];
  for (const candidate of candidates) {
    const resolved = path.join(root, candidate);
    if (fs.existsSync(resolved)) return resolved;
  }
  fail('cef-pdf executable was not found; pass its path as the first argument');
}

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

function request(baseUrl, method, pathname, body, extraHeaders = {}) {
  const data = body === undefined ? null : Buffer.from(JSON.stringify(body), 'utf8');
  const url = new URL(pathname, baseUrl);
  return new Promise((resolve, reject) => {
    const headers = data ? {
      'Content-Type': 'application/json',
      'Content-Length': data.length,
      ...extraHeaders
    } : extraHeaders;
    const outgoing = http.request(url, { method, headers, agent: false }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => resolve({
        status: response.statusCode,
        headers: response.headers,
        body: Buffer.concat(chunks)
      }));
    });
    outgoing.setTimeout(30000, () => outgoing.destroy(new Error(`${method} ${pathname} timed out`)));
    outgoing.on('error', reject);
    if (data) outgoing.write(data);
    outgoing.end();
  });
}

function jsonResponse(response, expectedStatus, label) {
  if (response.status !== expectedStatus) {
    fail(`${label} returned HTTP ${response.status}: ${response.body.toString('utf8')}`);
  }
  try {
    return JSON.parse(response.body.toString('utf8'));
  } catch (error) {
    fail(`${label} returned invalid JSON: ${error.message}`);
  }
}

async function getStatus(baseUrl) {
  return jsonResponse(await request(baseUrl, 'GET', '/api/status'), 200, 'status request');
}

async function convert(baseUrl, body, csrfToken) {
  const result = jsonResponse(await request(baseUrl, 'POST', '/api/convert', body, {
    'X-CSRF-Token': csrfToken
  }), 200, 'conversion');
  if (result.status !== 'success' || typeof result.downloadUrl !== 'string') {
    fail(`conversion did not return a download URL: ${JSON.stringify(result)}`);
  }
  return result;
}

async function consumeOnce(baseUrl, conversion, verify) {
  const first = await request(baseUrl, 'GET', conversion.downloadUrl);
  if (first.status !== 200) fail(`output download returned HTTP ${first.status}`);
  verify(first.body);
  const second = await request(baseUrl, 'GET', conversion.downloadUrl);
  const missing = jsonResponse(second, 404, 'second output download');
  if (!missing.error || missing.error.code !== 'not_found') fail('second output download did not report not_found');
}

async function waitForState(baseUrl, wanted, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let status;
  while (Date.now() < deadline) {
    status = await getStatus(baseUrl);
    if (status.state === wanted) return status;
    await delay(100);
  }
  fail(`worker did not reach '${wanted}'; last state was '${status && status.state}'`);
}

function verifyPdf(data) {
  if (data.length < 5 || data.toString('ascii', 0, 5) !== '%PDF-') fail('downloaded PDF signature is invalid');
  if (!data.subarray(Math.max(0, data.length - 1024)).toString('ascii').includes('%%EOF')) fail('downloaded PDF end marker is missing');
}

function verifyPng(data, width, height) {
  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  if (data.length < 24 || !data.subarray(0, 8).equals(signature)) fail('downloaded PNG signature is invalid');
  const actualWidth = data.readUInt32BE(16);
  const actualHeight = data.readUInt32BE(20);
  if (actualWidth !== width || actualHeight !== height) {
    fail(`downloaded PNG is ${actualWidth}x${actualHeight}, expected ${width}x${height}`);
  }
}

function jpegDimensions(data) {
  if (data.length < 4 || data[0] !== 0xff || data[1] !== 0xd8) fail('downloaded JPEG signature is invalid');
  const sof = new Set([0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf]);
  let offset = 2;
  while (offset < data.length) {
    while (offset < data.length && data[offset] !== 0xff) offset += 1;
    while (offset < data.length && data[offset] === 0xff) offset += 1;
    if (offset >= data.length) break;
    const marker = data[offset++];
    if (marker === 0xd8 || marker === 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (offset + 1 >= data.length) fail('downloaded JPEG segment is truncated');
    const length = data.readUInt16BE(offset);
    if (length < 2 || offset + length > data.length) fail('downloaded JPEG segment length is invalid');
    if (sof.has(marker)) return [data.readUInt16BE(offset + 5), data.readUInt16BE(offset + 3)];
    offset += length;
  }
  fail('downloaded JPEG has no supported SOF marker');
}

function verifyJpeg(data, width, height) {
  if (data[data.length - 2] !== 0xff || data[data.length - 1] !== 0xd9) fail('downloaded JPEG end marker is invalid');
  const dimensions = jpegDimensions(data);
  if (dimensions[0] !== width || dimensions[1] !== height) {
    fail(`downloaded JPEG is ${dimensions[0]}x${dimensions[1]}, expected ${width}x${height}`);
  }
}

function startServer(executable) {
  const child = spawn(process.execPath, [
    __filename,
    '--server-child',
    '--cef-pdf', executable,
    '--host', '127.0.0.1',
    '--port', '0',
    '--idle-timeout', '2'
  ], {
    cwd: root,
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe', 'ipc']
  });

  let stdout = '';
  let stderr = '';
  let listeningSettled = false;
  let resolveListening;
  let rejectListening;
  const listening = new Promise((resolve, reject) => {
    resolveListening = resolve;
    rejectListening = reject;
  });
  const closed = new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
  const timer = setTimeout(() => {
    if (!listeningSettled) {
      listeningSettled = true;
      rejectListening(new Error(`sample server did not announce a listening URL\n${stderr}`));
    }
  }, 10000);

  child.stdout.on('data', chunk => {
    stdout += chunk.toString('utf8');
    const match = /Node streamed server listening on (http:\/\/127\.0\.0\.1:\d+)/.exec(stdout);
    if (match && !listeningSettled) {
      listeningSettled = true;
      clearTimeout(timer);
      resolveListening(match[1]);
    }
  });
  child.stderr.on('data', chunk => {
    if (stderr.length < 131072) stderr += chunk.toString('utf8');
  });
  child.once('error', error => {
    if (!listeningSettled) {
      listeningSettled = true;
      clearTimeout(timer);
      rejectListening(error);
    }
  });
  child.once('close', (code, signal) => {
    if (!listeningSettled) {
      listeningSettled = true;
      clearTimeout(timer);
      rejectListening(new Error(`sample server exited before listening with code ${code}${signal ? ` (${signal})` : ''}\n${stderr}`));
    }
  });
  return { child, listening, closed, stderr: () => stderr };
}

async function stopServer(server) {
  if (server.child.exitCode !== null || server.child.signalCode !== null) return;
  if (server.child.connected) server.child.send({ command: 'shutdown' });
  else fail('sample server IPC channel closed before shutdown');
  const timeout = delay(10000).then(() => null);
  const result = await Promise.race([server.closed, timeout]);
  if (!result) {
    server.child.kill();
    await server.closed;
    fail('sample server did not shut down cleanly');
  }
  if (result.code !== 0) {
    fail(`sample server exited with code ${result.code}${result.signal ? ` (${result.signal})` : ''}\n${server.stderr()}`);
  }
}

async function run() {
  const executable = resolveExecutable(process.argv[2]);
  const html = fs.readFileSync(path.join(root, 'tests', 'fixtures', 'streamed-rich.html'), 'utf8');
  const server = startServer(executable);
  let testError;
  try {
    const baseUrl = await server.listening;
    const initialStatus = await getStatus(baseUrl);
    if (typeof initialStatus.csrfToken !== 'string' || initialStatus.csrfToken.length === 0) {
      fail('status response did not provide a CSRF token');
    }
    const csrfToken = initialStatus.csrfToken;
    const first = await convert(baseUrl, {
      html,
      format: 'png',
      capture: 'viewport',
      imageWidth: 256,
      imageHeight: 144
    }, csrfToken);
    const firstStatus = await getStatus(baseUrl);
    if (firstStatus.state !== 'running' || !Number.isInteger(firstStatus.pid) || firstStatus.generation < 1) {
      fail(`first worker status is invalid: ${JSON.stringify(firstStatus)}`);
    }

    const second = await convert(baseUrl, { html, format: 'pdf', margins: '10', backgrounds: true }, csrfToken);
    const secondStatus = await getStatus(baseUrl);
    if (secondStatus.state !== 'running') fail(`worker is not running after second conversion: ${secondStatus.state}`);
    if (secondStatus.generation !== firstStatus.generation || secondStatus.pid !== firstStatus.pid) {
      fail('the first two conversions did not use the same worker generation and PID');
    }

    await consumeOnce(baseUrl, first, data => verifyPng(data, 256, 144));
    await consumeOnce(baseUrl, second, verifyPdf);

    const idleStatus = await waitForState(baseUrl, 'stopped after inactivity', 15000);
    if (idleStatus.pid !== null || idleStatus.generation !== firstStatus.generation) {
      fail(`idle worker status is invalid: ${JSON.stringify(idleStatus)}`);
    }

    const third = await convert(baseUrl, {
      html,
      format: 'jpeg',
      capture: 'viewport',
      imageWidth: 200,
      imageHeight: 120,
      quality: 80
    }, csrfToken);
    const thirdStatus = await getStatus(baseUrl);
    if (thirdStatus.state !== 'running' || !Number.isInteger(thirdStatus.pid)) {
      fail(`restarted worker status is invalid: ${JSON.stringify(thirdStatus)}`);
    }
    if (thirdStatus.generation !== firstStatus.generation + 1) {
      fail('the third conversion did not start a new worker generation');
    }
    await consumeOnce(baseUrl, third, data => verifyJpeg(data, 200, 120));
    console.log(`node server lifecycle smoke passed: generation ${firstStatus.generation} PID ${firstStatus.pid}, then generation ${thirdStatus.generation} PID ${thirdStatus.pid}`);
  } catch (error) {
    testError = error;
  }

  try {
    await stopServer(server);
  } catch (error) {
    if (!testError) testError = error;
    else testError.message += `; shutdown also failed: ${error.message}`;
  }
  if (testError) throw testError;
}
