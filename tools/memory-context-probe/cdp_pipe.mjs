// Isolated Chromium test transport. No listening port, existing profile, or
// desktop input is used. Chromium's sandbox remains enabled.
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';

export class ChromePipe {
  static async launch(executable) {
    const profile = await mkdtemp(path.join(tmpdir(), 'llavon-marker-probe-'));
    const child = spawn(executable, [
      '--headless=new', '--disable-gpu', '--remote-debugging-pipe',
      `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
      '--disable-background-networking', '--disable-component-update',
      '--disable-default-apps', '--disable-extensions', '--disable-sync',
      '--password-store=basic', 'about:blank',
    ], { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'] });
    const client = new ChromePipe(child, profile);
    try {
      client.version = await client.call('Browser.getVersion');
      return client;
    } catch (error) {
      await client.close();
      throw new Error(`${error.message}\n${client.stderr}`);
    }
  }

  constructor(child, profile) {
    this.child = child;
    this.profile = profile;
    this.pending = new Map();
    this.listeners = new Set();
    this.sequence = 0;
    this.buffer = Buffer.alloc(0);
    this.stderr = '';
    this.exited = false;
    this.closed = new Promise(resolve => child.once('close', resolve));
    child.stderr.on('data', data => { this.stderr = (this.stderr + data).slice(-8000); });
    child.on('error', error => this.fail(error));
    child.once('exit', (code, signal) => {
      this.exited = true;
      this.fail(new Error(`Chromium exited: code=${code} signal=${signal}`));
    });
    child.stdio[3].on('error', error => this.fail(error));
    child.stdio[4].on('data', data => {
      this.buffer = Buffer.concat([this.buffer, data]);
      for (;;) {
        const end = this.buffer.indexOf(0);
        if (end === -1) break;
        const raw = this.buffer.subarray(0, end).toString('utf8');
        this.buffer = this.buffer.subarray(end + 1);
        try {
          const message = JSON.parse(raw);
          if (message.id) {
            const pending = this.pending.get(message.id);
            if (!pending) continue;
            this.pending.delete(message.id);
            clearTimeout(pending.timer);
            if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
            else pending.resolve(message.result);
          } else {
            for (const listener of this.listeners) listener(message);
          }
        } catch (error) { this.fail(error); }
      }
    });
  }

  fail(error) {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }

  call(method, params = {}, sessionId) {
    if (this.exited) return Promise.reject(new Error('Chromium already exited'));
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`DevTools timeout: ${method}`));
      }, 15000);
      this.pending.set(id, { resolve, reject, timer });
      this.child.stdio[3].write(JSON.stringify({ id, method, params, sessionId }) + '\0');
    });
  }

  async close() {
    if (!this.exited) {
      try { await this.call('Browser.close'); } catch {}
      const timer = setTimeout(() => this.child.kill('SIGKILL'), 3000);
      await this.closed;
      clearTimeout(timer);
    }
    this.fail(new Error('Browser closed'));
    // Only the exact directory returned by mkdtemp is removed.
    await rm(this.profile, { recursive: true, force: true });
  }
}
