'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const os = require('os');
const { URL } = require('url');
const { CefPdfClient } = require('./cef-pdf-client');

const BODY_LIMIT = 2 * 1024 * 1024;
const OUTPUT_TTL_MS = 10 * 60 * 1000;
const MAX_ACTIVE_CONVERSIONS = 4;
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const SESSION_ID = `${process.pid}-${crypto.randomBytes(8).toString('hex')}`;
const OUTPUT_ROOT = path.resolve(ROOT, '..', '..', 'build', 'node-streamed-server');
const OUTPUT_DIR = path.join(OUTPUT_ROOT, SESSION_ID);
const PROFILE_DIR = path.join(os.tmpdir(), `cef-pdf-node-streamed-${SESSION_ID}`);
const MEDIA_TYPES = { pdf: 'application/pdf', png: 'image/png', jpeg: 'image/jpeg', bmp: 'image/bmp' };
const EXTENSIONS = { pdf: 'pdf', png: 'png', jpeg: 'jpg', bmp: 'bmp' };
const STATIC_FILES = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/style.css': ['style.css', 'text/css; charset=utf-8']
};

function parseArguments(argv) {
  const defaults = process.platform === 'win32' ? [
    '../../build/vs17-x64-7680/src/Release/cef-pdf.exe',
    '../../src/Release/cef-pdf.exe',
    '../../build/src/Release/cef-pdf.exe'
  ] : [
    '../../build/src/cef-pdf',
    '../../src/cef-pdf'
  ];
  const detected = defaults.find(candidate => fs.existsSync(path.resolve(ROOT, candidate))) || defaults[0];
  const result = { cefPdf: process.env.CEF_PDF_PATH || detected, host: '127.0.0.1', port: 3000, idleTimeout: 60 };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const value = argv[index + 1];
    if (argument === '--cef-pdf' && value) result.cefPdf = value, index += 1;
    else if (argument === '--host' && value) result.host = value, index += 1;
    else if (argument === '--port' && value) result.port = parseInteger(value, 0, 65535, 'port'), index += 1;
    else if (argument === '--idle-timeout' && value) result.idleTimeout = parseNumber(value, 0, 86400, 'idle timeout'), index += 1;
    else if (argument === '--help') {
      process.stdout.write('Usage: node server.js [--cef-pdf PATH] [--host HOST] [--port PORT] [--idle-timeout SECONDS]\n');
      process.exit(0);
    } else throw new Error(`Unknown or incomplete argument: ${argument}`);
  }
  result.cefPdf = path.resolve(ROOT, result.cefPdf);
  return result;
}

function parseNumber(value, minimum, maximum, name) {
  const number = Number(value);
  if (!Number.isFinite(number) || number < minimum || number > maximum) throw new Error(`${name} is out of range`);
  return number;
}

function parseInteger(value, minimum, maximum, name) {
  const number = parseNumber(value, minimum, maximum, name);
  if (!Number.isInteger(number)) throw new Error(`${name} must be an integer`);
  return number;
}

const config = parseArguments(process.argv.slice(2));
fs.mkdirSync(OUTPUT_DIR, { recursive: true });
fs.mkdirSync(PROFILE_DIR, { recursive: true });
const client = new CefPdfClient(config.cefPdf, {
  cwd: path.dirname(config.cefPdf),
  graceMs: 3000,
  args: ['--streamed', '--javascript', `--profile=${PROFILE_DIR}`]
});
const outputs = new Map();
const activeArtifacts = new Set();
const csrfToken = crypto.randomBytes(32).toString('hex');
let activeRequests = 0;
let lastActivity = Date.now();
let idleTimeout = config.idleTimeout;
let idleTimer = null;
let closing = false;

function removeSessionDirectories() {
  fs.rmSync(OUTPUT_DIR, { recursive: true, force: true });
  fs.rmSync(PROFILE_DIR, { recursive: true, force: true });
}

function removeFile(file) {
  if (!file) return;
  fs.rm(file, { force: true }, () => {});
}

function discardOutput(token) {
  const item = outputs.get(token);
  if (!item) return;
  outputs.delete(token);
  clearTimeout(item.timer);
  removeFile(item.path);
  removeFile(item.snapshotPath);
}

function armIdleTimer() {
  clearTimeout(idleTimer);
  idleTimer = null;
  if (closing || idleTimeout === 0 || activeRequests > 0 || client.state !== 'running') return;
  const remaining = Math.max(0, (lastActivity + idleTimeout * 1000) - Date.now());
  idleTimer = setTimeout(() => {
    idleTimer = null;
    if (idleTimeout > 0 && activeRequests === 0 && Date.now() >= lastActivity + idleTimeout * 1000) {
      client.stop('inactivity').catch((error) => {
        client.state = 'failed';
        client.failure = `Idle shutdown failed: ${error.message}`;
        process.stderr.write(`${client.failure}\n`);
      });
    } else armIdleTimer();
  }, remaining);
  idleTimer.unref();
}

function pauseIdleTimer() {
  clearTimeout(idleTimer);
  idleTimer = null;
}

function sendJson(response, status, data) {
  const body = Buffer.from(JSON.stringify(data));
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': body.length,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff'
  });
  response.end(body);
}

function readJson(request) {
  return new Promise((resolve, reject) => {
    let size = 0;
    let tooLarge = false;
    const chunks = [];
    request.on('data', (chunk) => {
      size += chunk.length;
      if (size > BODY_LIMIT && !tooLarge) {
        tooLarge = true;
        reject(Object.assign(new Error('Request body exceeds 2 MiB'), { statusCode: 413 }));
      } else if (!tooLarge) chunks.push(chunk);
    });
    request.on('end', () => {
      if (tooLarge) return;
      try {
        const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('JSON body must be an object');
        resolve(value);
      } catch (error) {
        reject(Object.assign(new Error(`Invalid JSON: ${error.message}`), { statusCode: 400 }));
      }
    });
    request.on('error', reject);
  });
}

function requestError(statusCode, code, message) {
  return Object.assign(new Error(message), { statusCode, code });
}

function enforceSameOrigin(request) {
  const fetchSite = request.headers['sec-fetch-site'];
  if (fetchSite && fetchSite !== 'same-origin') {
    throw requestError(403, 'cross_origin_request', 'Cross-origin API requests are not allowed');
  }
  const origin = request.headers.origin;
  if (!origin) return;
  const host = request.headers.host;
  if (!host) throw requestError(403, 'cross_origin_request', 'Request host is missing');
  let requestOrigin;
  let localOrigin;
  try {
    requestOrigin = new URL(origin).origin;
    localOrigin = new URL(`http://${host}`).origin;
  } catch (_) {
    throw requestError(403, 'cross_origin_request', 'Request origin or host is invalid');
  }
  if (requestOrigin !== localOrigin) {
    throw requestError(403, 'cross_origin_request', 'Cross-origin API requests are not allowed');
  }
}

function enforceJsonPost(request) {
  const mediaType = String(request.headers['content-type'] || '').split(';', 1)[0].trim().toLowerCase();
  if (mediaType !== 'application/json') {
    throw requestError(415, 'unsupported_media_type', 'POST requests require application/json');
  }
  const supplied = request.headers['x-csrf-token'];
  const suppliedBuffer = typeof supplied === 'string' ? Buffer.from(supplied) : Buffer.alloc(0);
  const expectedBuffer = Buffer.from(csrfToken);
  if (suppliedBuffer.length !== expectedBuffer.length || !crypto.timingSafeEqual(suppliedBuffer, expectedBuffer)) {
    throw requestError(403, 'invalid_csrf_token', 'A valid CSRF token is required');
  }
}

function optionalInteger(value, minimum, maximum, name) {
  if (value === undefined || value === null || value === '') return undefined;
  return parseInteger(value, minimum, maximum, name);
}

function optionalNumber(value, minimum, maximum, name) {
  if (value === undefined || value === null || value === '') return undefined;
  return parseNumber(value, minimum, maximum, name);
}

function buildRender(body) {
  if (typeof body.html !== 'string' || body.html.length === 0) throw Object.assign(new Error('html is required'), { statusCode: 400 });
  const format = String(body.format || 'pdf').toLowerCase().replace('jpg', 'jpeg');
  if (!MEDIA_TYPES[format]) throw Object.assign(new Error('format must be pdf, png, jpeg, or bmp'), { statusCode: 400 });

  const key = crypto.randomUUID();
  const outputPath = path.join(OUTPUT_DIR, `${key}.${EXTENSIONS[format]}`);
  const options = {};
  const delay = optionalInteger(body.delay, 0, 2147483647, 'delay');
  const signalTimeout = optionalInteger(body.signalTimeout, 0, 2147483647, 'signal timeout');
  const commonWidth = optionalInteger(body.viewportWidth, 1, 32767, 'viewport width');
  const commonHeight = optionalInteger(body.viewportHeight, 1, 32767, 'viewport height');
  if (delay !== undefined) options.delay = delay;
  if (commonWidth !== undefined) options.viewWidth = commonWidth;
  if (commonHeight !== undefined) options.viewHeight = commonHeight;
  if (body.waitSignal) options.waitSignal = true;
  if (signalTimeout !== undefined) options.waitSignalTimeout = signalTimeout;

  let snapshotPath = null;
  if (body.saveHtml) {
    snapshotPath = path.join(OUTPUT_DIR, `${key}.snapshot.html`);
    options.saveHtml = snapshotPath;
    if (body.staticOnly) options.staticOnly = true;
  } else if (body.staticOnly) {
    throw Object.assign(new Error('staticOnly requires saveHtml'), { statusCode: 400 });
  }

  if (format === 'pdf') {
    const customWidth = optionalInteger(body.pageWidth, 1, 10000, 'page width');
    const customHeight = optionalInteger(body.pageHeight, 1, 10000, 'page height');
    if (body.pageSize === 'custom') {
      if (customWidth === undefined || customHeight === undefined) throw Object.assign(new Error('Custom page width and height are required'), { statusCode: 400 });
      options.size = `${customWidth}x${customHeight}`;
    } else options.size = String(body.pageSize || 'A4');
    if (body.margins) options.margin = String(body.margins);
    if (body.landscape) options.landscape = true;
    if (body.backgrounds) options.backgrounds = true;
    const scale = optionalInteger(body.scale, 1, 200, 'scale');
    if (scale !== undefined) options.scale = scale;
    if (body.headerFooter) options.headerFooter = true;
    if (body.headerTitle) options.headerTitle = String(body.headerTitle);
    if (body.footerUrl) options.footerUrl = String(body.footerUrl);
  } else {
    const capture = String(body.capture || 'full');
    if (capture !== 'full' && capture !== 'viewport') throw Object.assign(new Error('capture must be full or viewport'), { statusCode: 400 });
    options.capture = capture;
    const imageWidth = optionalInteger(body.imageWidth, 1, 32767, 'image width');
    const imageHeight = optionalInteger(body.imageHeight, 1, 32767, 'image height');
    if (imageWidth !== undefined) options.viewWidth = imageWidth;
    if (imageHeight !== undefined) options.viewHeight = imageHeight;
    if ((options.viewWidth || 1280) * (options.viewHeight || 720) > 100000000) {
      throw Object.assign(new Error('Image viewport exceeds 100 million pixels'), { statusCode: 400 });
    }
    if (format === 'jpeg') options.quality = optionalInteger(body.quality === '' ? undefined : (body.quality ?? 90), 0, 100, 'JPEG quality');
    if (format === 'jpeg' || format === 'bmp') options.imageBackground = String(body.imageBackground || '#ffffff');
  }

  return {
    key,
    format,
    outputPath,
    snapshotPath,
    packet: {
      id: `render-${crypto.randomUUID()}`,
      command: 'render',
      input: { type: 'html', content: body.html },
      output: { path: outputPath, format },
      options
    }
  };
}

async function convert(request, response) {
  if (closing) {
    request.resume();
    return sendJson(response, 503, { error: { code: 'shutting_down', message: 'Server is shutting down' } });
  }
  if (activeRequests >= MAX_ACTIVE_CONVERSIONS) {
    request.resume();
    return sendJson(response, 429, { error: { code: 'too_many_requests', message: 'Too many active conversions' } });
  }
  activeRequests += 1;
  pauseIdleTimer();
  let render = null;
  let accepted = false;
  try {
    const body = await readJson(request);
    if (closing) throw requestError(503, 'shutting_down', 'Server is shutting down');
    render = buildRender(body);
    activeArtifacts.add(render);
    accepted = true;
    const result = await client.request(render.packet);
    if (!result || result.status !== 'success') {
      removeFile(render.outputPath);
      removeFile(render.snapshotPath);
      const error = result && result.error ? result.error : { code: 'render_error', message: 'cef-pdf did not complete the render' };
      sendJson(response, 422, { status: 'error', error });
      return;
    }
    const stat = await fs.promises.stat(render.outputPath);
    if (!stat.isFile()) throw new Error('cef-pdf reported success without an output file');
    if (closing) throw requestError(503, 'shutting_down', 'Server is shutting down');
    const token = crypto.randomBytes(24).toString('hex');
    const item = {
      path: render.outputPath,
      snapshotPath: render.snapshotPath,
      mediaType: MEDIA_TYPES[render.format],
      name: `render-${render.key}.${EXTENSIONS[render.format]}`,
      timer: setTimeout(() => discardOutput(token), OUTPUT_TTL_MS)
    };
    item.timer.unref();
    outputs.set(token, item);
    sendJson(response, 200, {
      status: 'success',
      downloadUrl: `/api/output/${token}`,
      mediaType: item.mediaType,
      fileName: item.name,
      snapshotSaved: Boolean(render.snapshotPath)
    });
  } catch (error) {
    if (render) {
      removeFile(render.outputPath);
      removeFile(render.snapshotPath);
    }
    sendJson(response, error.statusCode || 502, {
      status: 'error',
      error: { code: error.code || 'worker_error', message: error.message }
    });
  } finally {
    if (render) activeArtifacts.delete(render);
    activeRequests -= 1;
    if (accepted) lastActivity = Date.now();
    armIdleTimer();
  }
}

function download(token, response) {
  const item = outputs.get(token);
  if (!item) return sendJson(response, 404, { error: { code: 'not_found', message: 'Output is unavailable or was already downloaded' } });
  outputs.delete(token);
  clearTimeout(item.timer);
  const stream = fs.createReadStream(item.path);
  stream.once('open', () => {
    response.writeHead(200, {
      'Content-Type': item.mediaType,
      'Content-Disposition': `attachment; filename="${item.name}"`,
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff'
    });
    stream.pipe(response);
  });
  stream.once('error', (error) => {
    if (!response.headersSent) sendJson(response, 404, { error: { code: 'output_error', message: error.message } });
    else response.destroy(error);
  });
  const cleanup = () => {
    removeFile(item.path);
    removeFile(item.snapshotPath);
  };
  response.once('close', cleanup);
  stream.once('close', cleanup);
}

async function route(request, response) {
  const url = new URL(request.url, `http://${request.headers.host || '127.0.0.1'}`);
  if (url.pathname.startsWith('/api/')) enforceSameOrigin(request);
  if (request.method === 'POST' && url.pathname.startsWith('/api/')) enforceJsonPost(request);
  if (request.method === 'GET' && STATIC_FILES[url.pathname]) {
    const [file, contentType] = STATIC_FILES[url.pathname];
    const data = await fs.promises.readFile(path.join(PUBLIC, file));
    response.writeHead(200, { 'Content-Type': contentType, 'Content-Length': data.length, 'X-Content-Type-Options': 'nosniff' });
    return response.end(data);
  }
  if (request.method === 'GET' && url.pathname === '/api/status') {
    return sendJson(response, 200, {
      ...client.status(),
      idleTimeout,
      activeRequests,
      maxActiveRequests: MAX_ACTIVE_CONVERSIONS,
      lastActivity: new Date(lastActivity).toISOString(),
      csrfToken
    });
  }
  if (request.method === 'POST' && url.pathname === '/api/settings') {
    const body = await readJson(request);
    idleTimeout = parseNumber(body.idleTimeout, 0, 86400, 'idle timeout');
    armIdleTimer();
    return sendJson(response, 200, { idleTimeout });
  }
  if (request.method === 'POST' && url.pathname === '/api/convert') return convert(request, response);
  if (request.method === 'GET' && url.pathname.startsWith('/api/output/')) {
    const token = url.pathname.slice('/api/output/'.length);
    if (!/^[a-f0-9]{48}$/.test(token)) return sendJson(response, 404, { error: { code: 'not_found', message: 'Output not found' } });
    return download(token, response);
  }
  sendJson(response, 404, { error: { code: 'not_found', message: 'Route not found' } });
}

const server = http.createServer((request, response) => {
  route(request, response).catch((error) => {
    if (!response.headersSent) sendJson(response, error.statusCode || 400, { error: { code: error.code || 'invalid_request', message: error.message } });
    else response.destroy(error);
  });
});

server.listen(config.port, config.host, () => {
  const address = server.address();
  process.stdout.write(`Node streamed server listening on http://${config.host}:${address.port}\n`);
});

async function shutdown() {
  if (closing) return;
  closing = true;
  clearTimeout(idleTimer);
  for (const token of [...outputs.keys()]) discardOutput(token);
  server.close();
  try {
    await client.stop('server shutdown');
  } finally {
    for (const render of activeArtifacts) {
      removeFile(render.outputPath);
      removeFile(render.snapshotPath);
    }
    for (const token of [...outputs.keys()]) discardOutput(token);
    removeSessionDirectories();
  }
}

function handleShutdownSignal() {
  shutdown()
    .catch((error) => process.stderr.write(`Shutdown failed: ${error.message}\n`))
    .finally(() => process.exit(0));
}

process.once('SIGINT', handleShutdownSignal);
process.once('SIGTERM', handleShutdownSignal);
process.once('exit', () => {
  if (client.child && client.child.exitCode === null) client.child.kill();
  try { removeSessionDirectories(); } catch (_) {}
});
