// Real Blink editing + read-only Linux memory experiment. Uses only a new
// headless Chromium profile and synthetic fields. Requires Node 18+.
import { execFile } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { promisify } from 'node:util';
import { ChromePipe } from './cdp_pipe.mjs';

const exec = promisify(execFile);
const [browser, scanner, output] = process.argv.slice(2);
if (!browser || !scanner || !output) {
  throw new Error('usage: node chromium_undo_probe.mjs CHROMIUM MEMORY_RESIDUE_PROBE OUTPUT_DIRECTORY');
}
await mkdir(output, { recursive: true });
const scratch = await mkdtemp(path.join(tmpdir(), 'llavon-undo-patterns-'));
const chrome = await ChromePipe.launch(browser);
const records = [];
const prefix = '文首：早餐吃蛋餅，現在想喝';
const suffix = '咖啡。下一句';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const emit = async record => {
  records.push(record);
  console.log(JSON.stringify(record));
  await writeFile(path.join(output, 'results.jsonl'), records.map(x => JSON.stringify(x)).join('\n') + '\n');
};

async function owned(pid) {
  for (let depth = 0; pid > 1 && depth < 64; ++depth) {
    if (pid === chrome.child.pid) return true;
    try {
      const status = await readFile(`/proc/${pid}/status`, 'utf8');
      pid = Number(status.match(/^PPid:\s+(\d+)/m)?.[1] ?? 0);
    } catch { return false; }
  }
  return false;
}

async function inspect(patternFile) {
  const { processInfo } = await chrome.call('SystemInfo.getProcessInfo');
  const results = [];
  for (const info of processInfo.filter(x => x.type === 'renderer')) {
    const pid = Number(info.id);
    if (!await owned(pid)) throw new Error('Refusing to inspect a process outside the test browser tree');
    const { stdout } = await exec(scanner, [String(pid), patternFile], { timeout: 12000, maxBuffer: 1024 * 1024 });
    results.push(JSON.parse(stdout));
  }
  if (!results.length) throw new Error('No renderer process found');
  return results;
}

try {
  await emit({ scenario: 'environment', ...chrome.version, sandbox_disabled: false,
    transport: 'private_devtools_pipe', memory_scope: 'owned_renderer_resident_anonymous_rw_private' });
  const targets = await chrome.call('Target.getTargets');
  const target = targets.targetInfos.find(x => x.type === 'page');
  if (!target) throw new Error('No test page');
  const { sessionId } = await chrome.call('Target.attachToTarget', { targetId: target.targetId, flatten: true });
  const call = (method, params = {}) => chrome.call(method, params, sessionId);
  const evaluate = async expression => {
    const result = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const key = async (name, code, keyCode, modifiers = 0) => {
    const params = { key: name, code, windowsVirtualKeyCode: keyCode, modifiers };
    await call('Input.dispatchKeyEvent', { type: 'keyDown', ...params });
    await call('Input.dispatchKeyEvent', { type: 'keyUp', ...params });
  };
  await call('Page.enable');
  await call('Emulation.setDeviceMetricsOverride', { width: 900, height: 220, deviceScaleFactor: 1, mobile: false });
  for (const kind of ['input', 'textarea', 'contenteditable']) {
    for (const encoding of ['ascii', 'invisible']) {
      await call('Page.navigate', { url: 'about:blank' });
      await evaluate(`new Promise(resolve => {
        if (document.readyState === 'complete') resolve();
        else window.addEventListener('load', resolve, {once:true});
      })`);
      const tag = kind === 'contenteditable' ? '<div id="field" contenteditable="true"></div>'
        : kind === 'input' ? '<input id="field">' : '<textarea id="field"></textarea>';
      await evaluate(`(() => {
        document.body.innerHTML = ${JSON.stringify('<style>body{margin:24px;background:white}#field{font:24px sans-serif;width:800px;min-height:100px;border:1px solid #888;padding:8px;box-sizing:border-box;}</style>' + tag)};
        window.field = document.getElementById('field');
        window.base = ${JSON.stringify(prefix + suffix)};
        window.caret = ${prefix.length};
        window.isContent = ${kind === 'contenteditable'};
        window.read = () => isContent ? field.textContent : field.value;
        window.select = (start, end) => {
          if (isContent) {
            const range = document.createRange();
            range.setStart(field.firstChild, start); range.setEnd(field.firstChild, end);
            const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
          } else field.setSelectionRange(start, end);
        };
        if (isContent) field.textContent = base; else field.value = base;
        field.focus(); select(caret, caret);
        window.inputEvents = 0; window.dirtyFrames = 0; window.frames = 0;
        field.addEventListener('input', () => ++inputEvents);
        const frame = () => { ++frames; if (read() !== base) ++dirtyFrames; requestAnimationFrame(frame); };
        requestAnimationFrame(frame);
        window.snapshot = () => ({matchesBaseline: read() === base, length: read().length,
          selectionStart: isContent ? getSelection().anchorOffset : field.selectionStart,
          selectionEnd: isContent ? getSelection().focusOffset : field.selectionEnd,
          inputEvents, dirtyFrames, frames, canUndo: document.queryCommandEnabled('undo'),
          canRedo: document.queryCommandEnabled('redo')});
      })()`);
      const entropy = randomBytes(16);
      // 128-bit nonce encoded as either ASCII hex or 128 invisible operators.
      // This is an experimental encoding, not a claim of layout neutrality.
      const marker = encoding === 'ascii' ? `ctx-${entropy.toString('hex')}`
        : [...entropy].map(byte => Array.from({ length: 8 }, (_, bit) =>
          byte & (1 << (7 - bit)) ? '\u2064' : '\u2063').join('')).join('');
      const patternFile = path.join(scratch, `${kind}-${encoding}.patterns`);
      const patterns = [
        ['marker_utf8', Buffer.from(marker, 'utf8')],
        ['marker_utf16', Buffer.from(marker, 'utf16le')],
        ['prefix_marker_utf8', Buffer.from(prefix + marker, 'utf8')],
        ['prefix_marker_utf16', Buffer.from(prefix + marker, 'utf16le')],
      ];
      await writeFile(patternFile, patterns.map(([label, bytes]) => `${label} ${bytes.toString('hex')}`).join('\n'));
      const before = await evaluate('snapshot()');
      const memoryBefore = await inspect(patternFile);
      const started = performance.now();
      await call('Input.insertText', { text: marker });
      // Select only the injected marker, then delete through Blink's native
      // Backspace editing path. No value assignment or whole-field rewrite.
      await evaluate(`select(caret, caret + ${marker.length})`);
      await key('Backspace', 'Backspace', 8);
      const roundTripMs = performance.now() - started;
      const afterDelete = await evaluate('snapshot()');
      if (!afterDelete.matchesBaseline) throw new Error(`Delete failed in ${kind}/${encoding}`);
      const memoryAfterDelete = await inspect(patternFile);
      await sleep(1000);
      const memoryAfterOneSecond = await inspect(patternFile);
      await key('z', 'KeyZ', 90, 2);
      const undo1 = await evaluate('snapshot()');
      // Verify the exact restored text after all residue scans are finished.
      // Reading it now cannot create a false positive in the earlier scans.
      undo1.matchesInserted = await evaluate('read()') === prefix + marker + suffix;
      await key('z', 'KeyZ', 90, 2);
      const undo2 = await evaluate('snapshot()');
      await emit({ scenario: 'insert_then_delete', kind, encoding,
        marker_code_units: marker.length, marker_sha256: createHash('sha256').update(marker).digest('hex'),
        insert_select_delete_round_trip_ms: roundTripMs, before, afterDelete, undo1, undo2,
        memoryBefore, memoryAfterDelete, memoryAfterOneSecond });

      // Separate visual experiment: deliberately keep the marker present long
      // enough to render. This does NOT measure the insert/delete race above.
      // Run with the native caret and with it hidden to isolate text layout.
      for (const caretStyle of ['auto', 'transparent']) {
        await evaluate(`field.style.caretColor = ${JSON.stringify(caretStyle)}; select(caret, caret)`);
        const capture = async stage => {
          await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
          const { data } = await call('Page.captureScreenshot', { format: 'png' });
          const bytes = Buffer.from(data, 'base64');
          await writeFile(path.join(output, `${kind}-${encoding}-${caretStyle}-${stage}.png`), bytes);
          return createHash('sha256').update(bytes).digest('hex');
        };
        const baseline = await capture('baseline');
        await evaluate('select(caret, caret)');
        const control = await capture('control');
        await call('Input.insertText', { text: marker });
        const inserted = await capture('inserted');
        await evaluate(`select(caret, caret + ${marker.length})`);
        const selected = await capture('selected');
        await key('Backspace', 'Backspace', 8);
        const deleted = await capture('deleted');
        await emit({ scenario: 'rendered_marker', kind, encoding, caretStyle,
          comparison: 'png_byte_equality', baselineControlEqual: baseline === control,
          insertedEqual: baseline === inserted, selectedEqual: baseline === selected,
          deletedEqual: baseline === deleted, restored: (await evaluate('snapshot()')).matchesBaseline,
          hashes: { baseline, control, inserted, selected, deleted } });
      }
    }
  }
} finally {
  await chrome.close();
  await rm(scratch, { recursive: true, force: true });
}
