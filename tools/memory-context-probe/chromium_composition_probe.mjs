import { randomBytes } from 'node:crypto';
import { ChromePipe } from './cdp_pipe.mjs';

const chrome = await ChromePipe.launch(process.argv[2]);
try {
  const { targetInfos } = await chrome.call('Target.getTargets');
  const { sessionId } = await chrome.call('Target.attachToTarget', {
    targetId: targetInfos.find(x => x.type === 'page').targetId, flatten: true,
  });
  const call = (method, params = {}) => chrome.call(method, params, sessionId);
  const evaluate = async expression => {
    const result = await call('Runtime.evaluate', { expression, returnByValue: true });
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const key = async (key, code, virtual, modifiers = 0) => {
    const params = { key, code, windowsVirtualKeyCode: virtual, modifiers };
    await call('Input.dispatchKeyEvent', { type: 'keyDown', ...params });
    await call('Input.dispatchKeyEvent', { type: 'keyUp', ...params });
  };
  for (const selected of [false, true]) {
    await call('Page.navigate', { url: 'about:blank' });
    await evaluate(`document.body.innerHTML='<textarea id="e"></textarea>';
      window.e=document.getElementById('e'); e.value='原始文件'; e.focus(); e.setSelectionRange(4,4);
      window.state=()=>({value:e.value, start:e.selectionStart, end:e.selectionEnd,
        undo:document.queryCommandEnabled('undo'),redo:document.queryCommandEnabled('redo')})`);
    await call('Input.insertText', { text: '測試' });
    await key('ArrowLeft', 'ArrowLeft', 37);
    await key('ArrowRight', 'ArrowRight', 39);
    if (selected) await evaluate('e.setSelectionRange(1,3)');
    const before = await evaluate('state()');
    const marker = [...randomBytes(16)].map(byte => Array.from({ length: 8 }, (_, i) =>
      byte & (1 << i) ? '\u2063' : '\u2064').join('')).join('');
    await call('Input.imeSetComposition', { text: marker, selectionStart: marker.length, selectionEnd: marker.length });
    const duringLength = await evaluate('e.value.length');
    await call('Input.imeSetComposition', { text: '', selectionStart: 0, selectionEnd: 0 });
    const cancelled = await evaluate('state()');
    await key('z', 'KeyZ', 90, 2);
    const undo1 = await evaluate('state()');
    await key('z', 'KeyZ', 90, 2);
    const undo2 = await evaluate('state()');
    console.log(JSON.stringify({ selected, before, duringLength, cancelled, undo1, undo2 }));
  }
} finally { await chrome.close(); }
