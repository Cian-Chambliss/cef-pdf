'use strict';

const { spawn } = require('child_process');
const crypto = require('crypto');

const MAX_HEADER_BYTES = 16 * 1024;
const MAX_FRAME_BYTES = 16 * 1024 * 1024;

function formatExit(code, signal) {
  if (signal) return `signal ${signal}`;
  if (process.platform === 'win32' && Number.isInteger(code) && code > 0x7fffffff) {
    return `code ${code} (0x${code.toString(16).toUpperCase().padStart(8, '0')})`;
  }
  return `code ${code}`;
}

class CefPdfClient {
  constructor(executable, options = {}) {
    this.executable = executable;
    this.args = options.args || ['--streamed', '--javascript'];
    this.cwd = options.cwd;
    this.graceMs = options.graceMs || 3000;
    this.child = null;
    this.state = 'stopped';
    this.generation = 0;
    this.failure = null;
    this.pending = new Map();
    this.buffer = Buffer.alloc(0);
    this.expectedLength = null;
    this.startPromise = null;
    this.stopPromise = null;
    this.exitWaiters = [];
  }

  status() {
    return {
      state: this.state,
      pid: this.child && this.child.exitCode === null ? this.child.pid : null,
      generation: this.generation,
      error: this.failure
    };
  }

  async start() {
    if (this.state === 'running') return;
    if (this.startPromise) return this.startPromise;
    if (this.stopPromise) await this.stopPromise;

    this.startPromise = new Promise((resolve, reject) => {
      this.state = 'starting';
      this.failure = null;
      this.buffer = Buffer.alloc(0);
      this.expectedLength = null;

      const child = spawn(this.executable, this.args, {
        cwd: this.cwd,
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true
      });
      this.child = child;
      let settled = false;

      child.stdout.on('data', (chunk) => this._receive(chunk));
      child.stderr.on('data', (chunk) => process.stderr.write(chunk));
      child.stdin.on('error', (error) => {
        if (this.child === child && this.state !== 'stopping') this._protocolFailure(error);
      });
      child.once('spawn', () => {
        settled = true;
        this.generation += 1;
        this.state = 'running';
        resolve();
      });
      child.once('error', (error) => {
        if (!settled) {
          settled = true;
          this.state = 'failed';
          this.failure = error.message;
          this.child = null;
          reject(error);
        }
      });
      child.once('exit', (code, signal) => this._onExit(child, code, signal));
    }).finally(() => {
      this.startPromise = null;
    });

    return this.startPromise;
  }

  async request(packet) {
    if (!packet || (typeof packet.id !== 'string' && typeof packet.id !== 'number')) {
      throw new Error('A string or numeric request id is required');
    }
    await this.start();
    return this._send(packet);
  }

  _send(packet) {
    const id = packet.id;
    if (this.pending.has(id)) return Promise.reject(new Error(`Duplicate request id: ${id}`));
    if (!this.child || !this.child.stdin.writable) return Promise.reject(new Error('cef-pdf is not writable'));

    const body = Buffer.from(JSON.stringify(packet), 'utf8');
    const frame = Buffer.concat([
      Buffer.from(`Content-Length: ${body.length}\r\nContent-Type: application/json\r\n\r\n`, 'ascii'),
      body
    ]);

    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.child.stdin.write(frame, (error) => {
        if (!error) return;
        const pending = this.pending.get(id);
        if (pending) {
          this.pending.delete(id);
          pending.reject(error);
        }
      });
    });
  }

  _receive(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    try {
      while (true) {
        if (this.expectedLength === null) {
          const end = this.buffer.indexOf('\r\n\r\n');
          if (end === -1) {
            if (this.buffer.length > MAX_HEADER_BYTES) throw new Error('cef-pdf frame header is too large');
            return;
          }
          if (end > MAX_HEADER_BYTES) throw new Error('cef-pdf frame header is too large');
          const header = this.buffer.subarray(0, end).toString('ascii');
          this.buffer = this.buffer.subarray(end + 4);
          const lengths = header.split('\r\n').filter((line) => /^content-length\s*:/i.test(line));
          if (lengths.length !== 1) throw new Error('cef-pdf frame has an invalid Content-Length header');
          const match = /^content-length\s*:\s*([0-9]+)\s*$/i.exec(lengths[0]);
          if (!match) throw new Error('cef-pdf frame has an invalid Content-Length value');
          this.expectedLength = Number(match[1]);
          if (!Number.isSafeInteger(this.expectedLength) || this.expectedLength > MAX_FRAME_BYTES) {
            throw new Error('cef-pdf frame body is too large');
          }
        }

        if (this.buffer.length < this.expectedLength) return;
        const body = this.buffer.subarray(0, this.expectedLength);
        this.buffer = this.buffer.subarray(this.expectedLength);
        this.expectedLength = null;
        const response = JSON.parse(body.toString('utf8'));
        if (!response || (typeof response.id !== 'string' && typeof response.id !== 'number')) {
          throw new Error('cef-pdf response is missing an id');
        }
        const pending = this.pending.get(response.id);
        if (pending) {
          this.pending.delete(response.id);
          pending.resolve(response);
        }
      }
    } catch (error) {
      this._protocolFailure(error);
    }
  }

  _protocolFailure(error) {
    this.failure = error.message;
    this._rejectPending(error);
    if (this.child && this.child.exitCode === null) this.child.kill();
  }

  _rejectPending(error) {
    for (const pending of this.pending.values()) pending.reject(error);
    this.pending.clear();
  }

  _onExit(child, code, signal) {
    if (this.child !== child) return;
    const stopping = this.state === 'stopping';
    this.child = null;
    this.buffer = Buffer.alloc(0);
    this.expectedLength = null;
    const detail = formatExit(code, signal);
    this._rejectPending(new Error(`cef-pdf exited with ${detail}`));
    if (!stopping) {
      this.state = 'failed';
      this.failure = `cef-pdf exited unexpectedly with ${detail}`;
    }
    for (const resolve of this.exitWaiters.splice(0)) resolve();
  }

  _waitForExit() {
    if (!this.child) return Promise.resolve();
    return new Promise((resolve) => this.exitWaiters.push(resolve));
  }

  async stop(reason = 'server shutdown') {
    if (this.stopPromise) return this.stopPromise;
    if (this.startPromise) {
      try { await this.startPromise; } catch (_) { return; }
    }
    if (!this.child) {
      if (reason === 'inactivity') this.state = 'stopped after inactivity';
      else if (this.state !== 'failed') this.state = 'stopped';
      return;
    }

    this.stopPromise = this._stop(reason).finally(() => {
      this.stopPromise = null;
    });
    return this.stopPromise;
  }

  async _stop(reason) {
    this.state = 'stopping';
    const child = this.child;
    const exit = this._waitForExit();
    const quitId = `quit-${crypto.randomUUID()}`;
    const graceful = this._send({ id: quitId, command: 'quit' })
      .catch(() => null)
      .then(() => exit);
    let timer;
    const timedOut = new Promise((resolve) => {
      timer = setTimeout(() => resolve(true), this.graceMs);
      timer.unref();
    });
    const forced = await Promise.race([graceful.then(() => false), timedOut]);
    clearTimeout(timer);
    if (forced && this.child === child && child.exitCode === null) {
      child.kill();
      await exit;
    }
    this.state = reason === 'inactivity' ? 'stopped after inactivity' : 'stopped';
    this.failure = null;
  }
}

module.exports = { CefPdfClient };
