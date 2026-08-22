#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const root = path.resolve(__dirname, '..', '..');
const outputDir = path.join(root, 'build', 'streamed-smoke', 'node');

function fail(message) {
  throw new Error(message);
}

function resolveExecutable(requested) {
  if (requested) {
    const resolved = path.resolve(requested);
    if (!fs.existsSync(resolved)) fail(`cef-pdf executable not found: ${resolved}`);
    return resolved;
  }
  const names = process.platform === 'win32' ? [
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
  for (const name of names) {
    const candidate = path.join(root, name);
    if (fs.existsSync(candidate)) return candidate;
  }
  fail('cef-pdf executable was not found; pass its path as the first argument');
}

function frame(packet) {
  const body = Buffer.from(JSON.stringify(packet), 'utf8');
  const header = Buffer.from(`Content-Length: ${body.length}\r\nContent-Type: application/json\r\n\r\n`, 'ascii');
  return Buffer.concat([header, body]);
}

function parseFrames(data) {
  const responses = [];
  let offset = 0;
  while (offset < data.length) {
    const headerEnd = data.indexOf('\r\n\r\n', offset, 'ascii');
    if (headerEnd < 0) fail('response has an incomplete header');
    if (headerEnd - offset > 16384) fail('response header exceeds 16 KiB');
    const header = data.toString('ascii', offset, headerEnd);
    const match = /^Content-Length:\s*(\d+)\s*$/im.exec(header);
    if (!match) fail('response has no valid Content-Length header');
    const length = Number(match[1]);
    const bodyStart = headerEnd + 4;
    if (bodyStart + length > data.length) fail('response body is shorter than Content-Length');
    responses.push(JSON.parse(data.toString('utf8', bodyStart, bodyStart + length)));
    offset = bodyStart + length;
  }
  return responses;
}

function pngDimensions(data) {
  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  if (data.length < 24 || !data.subarray(0, 8).equals(signature)) fail('PNG signature is invalid');
  return [data.readUInt32BE(16), data.readUInt32BE(20)];
}

function jpegDimensions(data) {
  if (data.length < 4 || data[0] !== 0xff || data[1] !== 0xd8) fail('JPEG signature is invalid');
  if (data[data.length - 2] !== 0xff || data[data.length - 1] !== 0xd9) fail('JPEG end marker is invalid');
  const sof = new Set([0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf]);
  let offset = 2;
  while (offset < data.length) {
    while (offset < data.length && data[offset] !== 0xff) offset++;
    while (offset < data.length && data[offset] === 0xff) offset++;
    if (offset >= data.length) break;
    const marker = data[offset++];
    if (marker === 0xd8 || marker === 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (offset + 1 >= data.length) fail('JPEG segment is truncated');
    const length = data.readUInt16BE(offset);
    if (length < 2 || offset + length > data.length) fail('JPEG segment length is invalid');
    if (sof.has(marker)) return [data.readUInt16BE(offset + 5), data.readUInt16BE(offset + 3)];
    offset += length;
  }
  fail('JPEG has no supported SOF marker');
}

async function run() {
  const executable = resolveExecutable(process.argv[2]);
  fs.rmSync(outputDir, { recursive: true, force: true });
  fs.mkdirSync(outputDir, { recursive: true });
  const html = fs.readFileSync(path.join(root, 'tests', 'fixtures', 'streamed-rich.html'), 'utf8');
  const svg = fs.readFileSync(path.join(root, 'tests', 'fixtures', 'streamed-rich.svg'), 'utf8');
  const requests = [
    { id: 'pdf-1', command: 'render', input: { type: 'html', content: html }, output: { path: 'build/streamed-smoke/node/page.pdf', format: 'pdf' }, options: { size: 'A4', margin: '10', backgrounds: true } },
    { id: 'png-1', command: 'render', input: { type: 'html', content: html }, output: { path: 'build/streamed-smoke/node/page.png', format: 'png' }, options: { capture: 'viewport', viewWidth: 360, viewHeight: 240 } },
    { id: 'jpeg-1', command: 'render', input: { type: 'svg', content: svg }, output: { path: 'build/streamed-smoke/node/page.jpg', format: 'jpeg' }, options: { capture: 'viewport', viewWidth: 320, viewHeight: 180, quality: 82, imageBackground: '#ffffff' } },
    { id: 'quit-1', command: 'quit' }
  ];

  const child = spawn(executable, ['--streamed', '--disable-gpu'], { cwd: root, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  const stdout = [];
  let stderr = '';
  child.stdout.on('data', chunk => stdout.push(chunk));
  child.stderr.on('data', chunk => { if (stderr.length < 65536) stderr += chunk.toString(); });

  const result = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal }));
  });
  for (const request of requests) child.stdin.write(frame(request));
  child.stdin.end();

  const timeout = setTimeout(() => child.kill(), 120000);
  const { code, signal } = await result;
  clearTimeout(timeout);
  if (code !== 0) fail(`cef-pdf exited with code ${code}${signal ? ` (${signal})` : ''}\n${stderr}`);

  const responses = parseFrames(Buffer.concat(stdout));
  if (responses.length !== 4) fail(`expected 4 responses, received ${responses.length}`);
  const expected = new Set(['pdf-1', 'png-1', 'jpeg-1', 'quit-1']);
  const seen = new Set();
  for (const response of responses) {
    if (!expected.has(String(response.id))) fail(`unexpected response id: ${response.id}`);
    if (seen.has(String(response.id))) fail(`duplicate response id: ${response.id}`);
    if (response.status !== 'success') fail(`request ${response.id} returned status '${response.status}': ${response.message || ''}`);
    seen.add(String(response.id));
  }
  if (seen.size !== expected.size) fail('one or more response IDs are missing');
  if (String(responses[responses.length - 1].id) !== 'quit-1') fail('quit response was not last');

  const pdf = fs.readFileSync(path.join(outputDir, 'page.pdf'));
  if (pdf.length < 5 || pdf.toString('ascii', 0, 5) !== '%PDF-') fail('PDF signature is invalid');
  if (!pdf.subarray(Math.max(0, pdf.length - 1024)).toString('ascii').includes('%%EOF')) fail('PDF end marker is missing');
  const png = pngDimensions(fs.readFileSync(path.join(outputDir, 'page.png')));
  if (png[0] !== 360 || png[1] !== 240) fail(`PNG dimensions are ${png[0]}x${png[1]}, expected 360x240`);
  const jpeg = jpegDimensions(fs.readFileSync(path.join(outputDir, 'page.jpg')));
  if (jpeg[0] !== 320 || jpeg[1] !== 180) fail(`JPEG dimensions are ${jpeg[0]}x${jpeg[1]}, expected 320x180`);
  console.log('streamed smoke passed: PDF, PNG 360x240, JPEG 320x180');
}

run().catch(error => {
  console.error(`streamed smoke failed: ${error.message}`);
  process.exitCode = 1;
});
