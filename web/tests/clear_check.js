'use strict';
/*
 * Focused, deterministic verification of the history clear flow on the REAL device.
 *
 * The 5-minute run reported count=1 right after clearing and no empty state. Both
 * would be the *correct* page behaviour if the 1.5 s poller added a fresh sample
 * between the clear and the assertion — this script removes that race by pausing
 * the poller, then clears, then asserts, then resumes polling and proves that
 * collection continues.
 */
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const URL_BASE = 'http://192.168.1.111/';
const CDP_PORT = 9337;
const PROFILE = path.join(require('os').tmpdir(), 'wstest', 'chrome-profile-clear');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0;
const failures = [];
const check = (name, ok, detail) => {
  if (ok) { passed += 1; console.log('  [OK]   ' + name); } else {
    failures.push(name); console.log('  [FAIL] ' + name + ' -> ' + JSON.stringify(detail));
  }
};

function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(wsUrl);
    let nextId = 1;
    const pending = new Map();
    socket.addEventListener('message', (event) => {
      const m = JSON.parse(event.data);
      if (m.id && pending.has(m.id)) {
        const { resolve: done, reject: fail } = pending.get(m.id);
        pending.delete(m.id);
        if (m.error) { fail(new Error(JSON.stringify(m.error))); } else { done(m.result); }
      }
    });
    socket.addEventListener('error', reject);
    socket.addEventListener('open', () => resolve({
      send(method, params) {
        const id = nextId++;
        return new Promise((done, fail) => {
          pending.set(id, { resolve: done, reject: fail });
          socket.send(JSON.stringify({ id, method, params: params || {} }));
        });
      },
      close() { socket.close(); },
    }));
  });
}

(async function main() {
  const chrome = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
  fs.mkdirSync(PROFILE, { recursive: true });
  const browser = spawn(chrome, ['--headless=new', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-extensions', '--mute-audio',
    '--remote-debugging-port=' + CDP_PORT, '--user-data-dir=' + PROFILE, 'about:blank'],
  { stdio: 'ignore' });
  let target = null;
  for (let i = 0; i < 80 && !target; i += 1) {
    await sleep(250);
    try {
      const list = await (await fetch('http://127.0.0.1:' + CDP_PORT + '/json/list')).json();
      target = list.find((e) => e.type === 'page');
    } catch (error) { /* wait */ }
  }
  const cdp = await connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  const evaluate = async (expr) => {
    const r = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) { throw new Error(JSON.stringify(r.exceptionDetails)); }
    return r.result.value;
  };

  await cdp.send('Page.navigate', { url: URL_BASE });
  for (let i = 0; i < 80; i += 1) {
    await sleep(200);
    try { if (await evaluate('document.readyState === "complete" && typeof uiState !== "undefined"')) { break; } }
    catch (error) { /* wait */ }
  }
  await sleep(600);
  await evaluate('document.getElementById("lang-en").click()');
  await sleep(2500);
  await evaluate('setActiveMode("history")');
  await sleep(600);

  /* collect real points first */
  for (let i = 0; i < 20 && (await evaluate('history.count')) < 3; i += 1) { await sleep(1000); }
  const collected = await evaluate('history.count');
  check('history collected real device points before clearing', collected >= 3, collected);

  /* pause the poller so the clear result is not raced by a fresh sample */
  await evaluate('clearTimeout(uiState.timerId); uiState.timerId = null; window.__savedInterval = APP_CONFIG.pollIntervalMs; APP_CONFIG.pollIntervalMs = 600000;');
  await sleep(300);
  const countBefore = await evaluate('history.count');
  const buttonLabel = await evaluate('document.querySelector(\'[data-role="clear"]\').textContent');
  await evaluate('document.querySelector(\'[data-role="clear"]\').click()');
  await sleep(300);
  const armedLabel = await evaluate('document.querySelector(\'[data-role="clear"]\').textContent');
  await evaluate('document.querySelector(\'[data-role="clear"]\').click()');
  await sleep(500);
  const countAfter = await evaluate('history.count');
  const emptyVisible = await evaluate('!document.querySelector(\'[data-role="empty"]\').hidden');
  const summary = await evaluate('document.querySelector(\'[data-role="summary"]\').textContent');

  console.log('  clear button: "' + buttonLabel + '" -> armed: "' + armedLabel + '"');
  check('two-step confirm arms the button', armedLabel !== buttonLabel, { buttonLabel, armedLabel });
  check('clear history empties the buffer (poller paused)', countAfter === 0,
    { before: countBefore, after: countAfter });
  check('empty state is shown after clearing', emptyVisible === true, { emptyVisible, summary });
  check('summary reports the empty/collecting state', /Collecting|Собираем/.test(summary), summary);

  /* resume polling: collection must continue and grow again */
  await evaluate('APP_CONFIG.pollIntervalMs = window.__savedInterval; uiState.timerId = setTimeout(pollOnce, APP_CONFIG.pollIntervalMs);');
  const grew = await (async () => {
    for (let i = 0; i < 20; i += 1) {
      await sleep(1000);
      if ((await evaluate('history.count')) >= 2) { return await evaluate('history.count'); }
    }
    return await evaluate('history.count');
  })();
  check('collection resumes after clearing (real device samples)', grew >= 2, grew);
  check('empty state hides once points exist again',
    (await evaluate('!document.querySelector(\'[data-role="empty"]\').hidden')) === false, grew);

  console.log('');
  console.log('clear-history verification passed: ' + passed + ', failed: ' + failures.length);
  console.log(failures.length === 0 ? 'CLEAR FLOW OK' : 'CLEAR FLOW FAILED: ' + failures.join(' | '));
  browser.kill();
  process.exit(failures.length === 0 ? 0 : 1);
})().catch((e) => { console.error('crashed: ' + (e && e.stack ? e.stack : e)); process.exit(2); });
