'use strict';
/*
 * Final end-to-end QA against the REAL device (http://192.168.1.111/).
 *
 * Headless Chrome over CDP — no npm dependencies, no mocks: the page comes from
 * the ESP32, the data comes from /api/weather on the same board.
 *
 * Usage: node device_qa.js [url] [--realtime-minutes N] [--fast]
 *   --fast  shortens the realtime observation (documented in the report)
 */
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const URL_BASE = (process.argv[2] && !process.argv[2].startsWith('--')) ? process.argv[2] : 'http://192.168.1.111/';
const API_URL = new URL('/api/weather', URL_BASE).toString();
const FAST = process.argv.includes('--fast');
const realtimeIdx = process.argv.indexOf('--realtime-minutes');
const REALTIME_MINUTES = realtimeIdx >= 0 ? Number(process.argv[realtimeIdx + 1])
  : (FAST ? 0.5 : 5);
const CDP_PORT = 9336;
const PROFILE = path.join(require('os').tmpdir(), 'wstest', 'chrome-profile-qa');

let passed = 0;
const failures = [];
function check(name, ok, detail) {
  if (ok) { passed += 1; console.log('  [OK]   ' + name); return true; }
  failures.push(name);
  console.log('  [FAIL] ' + name + (detail === undefined ? '' : ' -> ' + JSON.stringify(detail)));
  return false;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(wsUrl);
    let nextId = 1;
    const pending = new Map();
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error') {
        consoleErrors.push((message.params.args || []).map((a) => a.value || a.description || '').join(' '));
      }
      if (message.method === 'Runtime.exceptionThrown') {
        consoleErrors.push('exception: ' + JSON.stringify(message.params.exceptionDetails.text || ''));
      }
      if (message.id && pending.has(message.id)) {
        const { resolve: done, reject: fail } = pending.get(message.id);
        pending.delete(message.id);
        if (message.error) { fail(new Error(JSON.stringify(message.error))); } else { done(message.result); }
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
const consoleErrors = [];

async function apiSnapshot() {
  /* The board's HTTP server accepts only a couple of sockets, so a single fetch
     can fail while another client is being served: retry instead of crashing. */
  let lastError = null;
  for (let attempt = 0; attempt < 6; attempt += 1) {
    try {
      const response = await fetch(API_URL, { cache: 'no-store' });
      if (!response.ok) { throw new Error('API ' + response.status); }
      return await response.json();
    } catch (error) {
      lastError = error;
      await sleep(1200);
    }
  }
  throw new Error('API unreachable after 6 attempts: ' + (lastError && lastError.message));
}

function num(text) { const v = parseFloat(String(text).replace(',', '.')); return Number.isFinite(v) ? v : null; }

(async function main() {
  const chrome = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
  fs.mkdirSync(PROFILE, { recursive: true });
  const browser = spawn(chrome, [
    '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--mute-audio',
    '--remote-debugging-port=' + CDP_PORT, '--user-data-dir=' + PROFILE, 'about:blank',
  ], { stdio: 'ignore' });

  let target = null;
  for (let attempt = 0; attempt < 80 && !target; attempt += 1) {
    await sleep(250);
    try {
      const list = await (await fetch('http://127.0.0.1:' + CDP_PORT + '/json/list')).json();
      target = list.find((entry) => entry.type === 'page');
    } catch (error) { /* not ready */ }
  }
  if (!target) { throw new Error('Chrome DevTools endpoint did not come up'); }

  const cdp = await connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Network.enable');
  await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
    source: 'window.__fetchLog = []; const __f = window.fetch;' +
      'window.fetch = function () { window.__fetchLog.push(String(Array.prototype.slice.call(arguments)[0])); return __f.apply(this, arguments); };',
  });

  const evaluate = async (expression) => {
    const result = await cdp.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) { throw new Error('page eval failed: ' + JSON.stringify(result.exceptionDetails)); }
    return result.result.value;
  };
  const setViewport = (width, height) => cdp.send('Emulation.setDeviceMetricsOverride',
    { width, height: height || 900, deviceScaleFactor: 1, mobile: width < 768 });
  const waitReady = async () => {
    for (let attempt = 0; attempt < 80; attempt += 1) {
      await sleep(200);
      try { if (await evaluate('document.readyState === "complete" && typeof uiState !== "undefined"')) { return true; } }
      catch (error) { /* loading */ }
    }
    return false;
  };

  console.log('device QA: ' + URL_BASE + '  (api ' + API_URL + ', realtime ' + REALTIME_MINUTES + ' min)');
  await setViewport(1024, 900);
  await cdp.send('Page.navigate', { url: URL_BASE });
  await waitReady();
  await sleep(800);

  /* ---------------------------------------------------- §2 language selector */
  console.log('== language selection (real page, real device) ==');
  check('language screen is the first screen', await evaluate('!document.getElementById("language-screen").hidden') === true);
  check('dashboard hidden before the choice', await evaluate('document.getElementById("app-shell").hidden') === true);
  check('locale unset on start', await evaluate('uiState.locale') === null);
  check('no /api/weather request before the choice',
    await evaluate('window.__fetchLog.filter(u => u.indexOf("/api/weather") >= 0).length') === 0,
    await evaluate('JSON.stringify(window.__fetchLog)'));
  check('RU/EN buttons with locale ids',
    await evaluate('Array.from(document.querySelectorAll(".langbutton[data-locale]")).map(b => b.dataset.locale).join()') === 'ru,en');

  /* ------------------------------------------------------- §3 RU dashboard */
  console.log('== RU dashboard (§3, §5, §9) ==');
  const snapshot = await apiSnapshot();
  await evaluate('document.getElementById("lang-ru").click()');
  await sleep(2500);
  check('locale ru after the click', await evaluate('uiState.locale') === 'ru');
  check('device ONLINE', await evaluate('uiState.connection') === 'ONLINE',
    await evaluate('document.getElementById("connection-label").textContent'));
  check('tabs are Russian', await evaluate('document.getElementById("tab-horizontal").textContent') === 'Горизонтальные');
  check('5 tabs present', await evaluate('document.querySelectorAll(\'[role="tab"]\').length') === 5);

  const readMetrics = async () => JSON.parse(await evaluate(
    'JSON.stringify(uiState.uiModel.metrics.map(m => ({k: m.key, v: m.display, s: m.dataState, role: m.colorRole, icon: m.icon, hasData: m.hasData})))'));

  let metrics = await readMetrics();
  check('three metrics with real values (not —)', metrics.every((m) => m.v !== '—' && m.v !== '') && metrics.length === 3, metrics);
  check('all metrics FRESH', metrics.every((m) => m.s === 'FRESH'), metrics);
  const expectedRole = (key, v) => {
    if (key === 'temperature') { return v < 18 ? 'temp-cold' : (v > 26 ? 'temp-hot' : 'temp-comfort'); }
    if (key === 'humidity') { return v < 30 ? 'humidity-low' : (v > 60 ? 'humidity-high' : 'humidity-comfort'); }
    return v < 1000 ? 'pressure-low' : (v > 1020 ? 'pressure-high' : 'pressure-normal');
  };
  const roleMismatch = metrics.filter((m) => m.role !== expectedRole(m.k, Number(String(m.v).replace(',', '.'))));
  check('semantic roles follow the shared thresholds for the reported values',
    roleMismatch.length === 0,
    roleMismatch.map((m) => m.k + '=' + m.v + ' -> ' + m.role + ' (expected ' + expectedRole(m.k, Number(String(m.v).replace(',', '.'))) + ')'));
  check('icons present for all metrics', metrics.every((m) => m.icon && m.icon.length > 0), metrics.map((m) => m.icon));
  const valueOf = (list, key) => num((list.find((m) => m.k === key) || {}).v);
  check('temperature matches the API snapshot (±0.2 °C)',
    Math.abs(valueOf(metrics, 'temperature') - snapshot.temperature_c) <= 0.2,
    { web: valueOf(metrics, 'temperature'), api: snapshot.temperature_c });
  check('humidity matches the API snapshot (±1 %)',
    Math.abs(valueOf(metrics, 'humidity') - snapshot.humidity_pct) <= 1,
    { web: valueOf(metrics, 'humidity'), api: snapshot.humidity_pct });
  check('pressure matches the API snapshot (±0.5 hPa)',
    Math.abs(valueOf(metrics, 'pressure') - snapshot.pressure_hpa) <= 0.5,
    { web: valueOf(metrics, 'pressure'), api: snapshot.pressure_hpa });

  /* the five modes on the device */
  const modes = ['numeric', 'horizontal', 'bars', 'gauges', 'history'];
  const apiNow = await apiSnapshot();
  for (const mode of modes) {
    await evaluate('setActiveMode("' + mode + '")');
    await sleep(350);
    const state = JSON.parse(await evaluate('JSON.stringify({active: uiState.activeMode,' +
      'visible: Array.from(document.querySelectorAll(\'[role="tabpanel"]\')).filter(p => !p.hidden).map(p => p.dataset.mode),' +
      'selected: Array.from(document.querySelectorAll(\'[role="tab"]\')).filter(t => t.getAttribute("aria-selected") === "true").map(t => t.dataset.mode),' +
      'icons: document.querySelectorAll(\'#panel-' + mode + ' svg.icon, #panel-' + mode + ' .gauge__icon, #panel-' + mode + ' .card__icon svg\').length,' +
      'values: Array.from(document.querySelectorAll(\'#panel-' + mode + ' [data-role="value"]\')).map(n => n.textContent),' +
      'states: Array.from(document.querySelectorAll(\'#panel-' + mode + ' [data-metric] [data-role="state"], #panel-' + mode + ' [data-metric] .state\')).map(n => n.textContent).slice(0,3)})'));
    check('mode ' + mode + ': active tab', state.active === mode && state.selected.join() === mode, state);
    check('mode ' + mode + ': only its panel visible', state.visible.join() === mode, state.visible);
    if (mode !== 'history') {
      check('mode ' + mode + ': values rendered', state.values.length === 3 && state.values.every((v) => v && v !== '—'), state.values);
      check('mode ' + mode + ': icon present', state.icons > 0, state.icons);
      check('mode ' + mode + ': state badges say FRESH',
        state.states.length === 0 || state.states.every((s) => /FRESH|СВЕЖО/.test(s)), state.states);
    } else {
      check('mode history: chart present', await evaluate('!!document.querySelector(\'#panel-history [data-role="chart-svg"]\')'));
      check('mode history: lanes rendered', await evaluate('document.querySelectorAll(\'#panel-history [data-role="chart-svg"] polyline, #panel-history [data-role="chart-svg"] path\').length') > 0);
    }
    void apiNow;
  }

  /* ------------------------------------------------- §9/§20 cross-check again */
  const apiAfter = await apiSnapshot();
  await evaluate('setActiveMode("numeric")');
  await sleep(1800);
  metrics = await readMetrics();
  check('cross-check: temperature still matches the API (Web == ESP32)',
    Math.abs(valueOf(metrics, 'temperature') - apiAfter.temperature_c) <= 0.3,
    { web: valueOf(metrics, 'temperature'), api: apiAfter.temperature_c });
  check('cross-check: humidity still matches (DHT11)',
    Math.abs(valueOf(metrics, 'humidity') - apiAfter.humidity_pct) <= 1,
    { web: valueOf(metrics, 'humidity'), api: apiAfter.humidity_pct });
  check('cross-check: pressure still matches (BMP280)',
    Math.abs(valueOf(metrics, 'pressure') - apiAfter.pressure_hpa) <= 0.5,
    { web: valueOf(metrics, 'pressure'), api: apiAfter.pressure_hpa });

  /* ------------------------------------------------ §6 history on the device */
  console.log('== history (§6) ==');
  const presets = JSON.parse(await evaluate('JSON.stringify(HISTORY_CONFIG.presets.map(p => p.id))'));
  check('all 11 retention presets exist',
    presets.join() === '10s,30s,1m,5m,15m,30m,1h,3h,6h,12h,24h', presets);
  await evaluate('setActiveMode("history")');
  await sleep(400);
  const waitPoints = async (minPoints, timeoutMs) => {
    const deadline = Date.now() + timeoutMs;
    let count = 0;
    while (Date.now() < deadline) {
      count = await evaluate('history.count');
      if (count >= minPoints) { return count; }
      await sleep(1000);
    }
    return count;
  };
  const points5m = await waitPoints(4, 40000);
  check('history collects real points from the device API', points5m >= 4, points5m);
  const histInfo = JSON.parse(await evaluate('JSON.stringify({count: history.count, capacity: history.capacity,' +
    'total: history.totalSamples, last: history.time[history.count-1] - history.time[history.count-2]})'));
  check('history timestamps increase', typeof histInfo.last === 'number' && histInfo.last > 0, histInfo);
  const laneSeries = JSON.parse(await evaluate('JSON.stringify(Array.from(document.querySelectorAll(\'#panel-history [data-role="chart-svg"] polyline, #panel-history [data-role="chart-svg"] path\')).map(n => (n.getAttribute("points") || n.getAttribute("d") || "").split(/[ ,M]/).filter(Boolean).length))'));
  check('all three lanes draw the collected points', laneSeries.length >= 3 && laneSeries.every((n) => n > 3), laneSeries);
  check('history summarises retention and step', /Хранение|Retention/.test(await evaluate('document.querySelector(\'#panel-history [data-role="summary"]\').textContent')));

  /* retention change → buffer rebuilt, still collecting */
  await evaluate('historyConfigure("1m", Date.now())');
  await sleep(2500);
  const afterRetention = JSON.parse(await evaluate('JSON.stringify({id: history.presetId, capacity: history.capacity, count: history.count})'));
  check('retention change rebuilds the buffer', afterRetention.capacity <= 34 && afterRetention.count >= 1, afterRetention);

  /* tooltip via a real mouse move over the chart */
  const chartBox = JSON.parse(await evaluate('JSON.stringify((function(){const r=document.querySelector(\'#panel-history [data-role="chart"]\').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})())'));
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: chartBox.x, y: chartBox.y, buttons: 0 });
  await sleep(400);
  check('chart tooltip appears on mouse move', await evaluate('!document.querySelector(\'[data-role="tooltip"]\').hidden'),
    await evaluate('document.querySelector(\'[data-role="tooltip"]\').textContent'));

  /* clear history → empty state.
     The page polls the device every 1.5 s, so the poller is paused for this check:
     otherwise a fresh real sample lands between the click and the assertion (which
     is correct behaviour, not a defect — see clear_check.js). */
  await evaluate('clearTimeout(uiState.timerId); uiState.timerId = null; window.__qaInterval = APP_CONFIG.pollIntervalMs; APP_CONFIG.pollIntervalMs = 600000;');
  await sleep(300);
  const clearLabel = await evaluate('document.querySelector(\'[data-role="clear"]\').textContent');
  await evaluate('document.querySelector(\'[data-role="clear"]\').click()');
  await sleep(300);
  const armedLabel = await evaluate('document.querySelector(\'[data-role="clear"]\').textContent');
  check('clear history asks for confirmation first', armedLabel !== clearLabel, { clearLabel, armedLabel });
  await evaluate('document.querySelector(\'[data-role="clear"]\').click()');
  await sleep(500);
  check('clear history empties the buffer', await evaluate('history.count') === 0, await evaluate('history.count'));
  check('empty state is visible after clearing', await evaluate('!document.querySelector(\'[data-role="empty"]\').hidden'));
  /* resume the real poller and prove collection continues on the device */
  await evaluate('APP_CONFIG.pollIntervalMs = window.__qaInterval; uiState.timerId = setTimeout(pollOnce, APP_CONFIG.pollIntervalMs);');
  let resumed = 0;
  for (let attempt = 0; attempt < 20; attempt += 1) {
    await sleep(1000);
    resumed = await evaluate('history.count');
    if (resumed >= 2) { break; }
  }
  check('collection resumes from the device after clearing', resumed >= 2, resumed);
  await evaluate('historyConfigure("5m", Date.now())');
  await sleep(2500);

  /* --------------------------------------------- §5 EN + no untranslated text */
  console.log('== EN dashboard (§2, §5) ==');
  await evaluate('localStorage.clear(); sessionStorage.clear();');
  await cdp.send('Page.reload');
  await waitReady();
  await sleep(800);
  check('reload shows the language selector again (no persistence)',
    await evaluate('!document.getElementById("language-screen").hidden') === true);
  check('reload leaves the locale unset', await evaluate('uiState.locale') === null);
  await evaluate('document.getElementById("lang-en").click()');
  await sleep(2500);
  check('locale en after the click', await evaluate('uiState.locale') === 'en');
  check('tabs are English', await evaluate('document.getElementById("tab-horizontal").textContent') === 'HORIZONTAL');
  check('no Cyrillic in the English dashboard text',
    await evaluate('!/[А-Яа-яЁё]/.test(document.body.innerText)'),
    await evaluate('(document.body.innerText.match(/[А-Яа-яЁё]+/g)||[]).slice(0,6).join(",")'));
  metrics = await readMetrics();
  check('numbers and units unchanged in EN', metrics.every((m) => m.v !== '' && m.v !== '—'), metrics.map((m) => m.v));

  /* --------------------------------------------------- §4 realtime stability */
  console.log('== realtime observation (' + REALTIME_MINUTES + ' min) ==');
  const before = JSON.parse(await evaluate('JSON.stringify({fetches: window.__fetchLog.length, polls: uiState.pollCount,' +
    'connection: uiState.connection, uptime: uiState.uiModel.uptimeMs,' +
    'values: uiState.uiModel.metrics.map(m => m.value)})'));
  const consoleErrorsBefore = consoleErrors.length;
  await sleep(REALTIME_MINUTES * 60 * 1000);
  const after = JSON.parse(await evaluate('JSON.stringify({fetches: window.__fetchLog.length, polls: uiState.pollCount,' +
    'connection: uiState.connection, uptime: uiState.uiModel.uptimeMs,' +
    'values: uiState.uiModel.metrics.map(m => m.value),' +
    'urls: Array.from(new Set(window.__fetchLog)).slice(0,5),' +
    'external: performance.getEntriesByType("resource").filter(e => !e.name.startsWith(location.origin)).length})'));
  const minutes = Math.max(REALTIME_MINUTES, 0.01);
  const expected = Math.floor(minutes * 60 * 1000 / 1500);
  const delta = after.fetches - before.fetches;
  check('polling keeps running (requests ≈ 1 per 1.5 s)', delta >= expected * 0.7 && delta <= expected * 1.3 + 4,
    { delta, expected });
  check('no request accumulation (bounded by the poll cadence)', after.fetches <= before.fetches + expected * 1.3 + 6, { before, after });
  check('stays ONLINE for the whole window', after.connection === 'ONLINE', after.connection);
  check('device uptime grows', after.uptime > before.uptime, { before: before.uptime, after: after.uptime });
  check('values keep updating (no freeze)', after.polls > before.polls, { before: before.polls, after: after.polls });
  const origin = new URL(URL_BASE).origin;
  check('all requests go to the device origin /api/weather',
    after.urls.length > 0 && after.urls.every((u) => u.indexOf('/api/weather') >= 0 &&
      (u.startsWith('/') || u.indexOf(origin) === 0)),
    after.urls);
  check('no external resources requested', after.external === 0, after.external);
  check('no console errors during the window', consoleErrors.length === consoleErrorsBefore,
    consoleErrors.slice(consoleErrorsBefore).slice(0, 3));

  /* ------------------------------------------------- §7 reduced motion + §8 responsive */
  console.log('== reduced motion + responsive (§7, §8) ==');
  await cdp.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: 'reduce' }] });
  await sleep(500);
  const reduced = JSON.parse(await evaluate('JSON.stringify({media: matchMedia("(prefers-reduced-motion: reduce)").matches,' +
    'visible: !document.getElementById("app-shell").hidden, errors: 0})'));
  check('page renders with prefers-reduced-motion: reduce', reduced.media === true && reduced.visible === true, reduced);
  await cdp.send('Emulation.setEmulatedMedia', { features: [] });

  const widths = [320, 375, 430, 768, 1024, 1280, 1920];
  for (const width of widths) {
    await setViewport(width, width < 500 ? 720 : 900);
    await sleep(400);
    const layout = JSON.parse(await evaluate('JSON.stringify({w: innerWidth, docScroll: document.documentElement.scrollWidth,' +
      'bodyScroll: document.body.scrollWidth, tabsScroll: document.getElementById("tabs").scrollWidth,' +
      'tabsClient: document.getElementById("tabs").clientWidth,' +
      'overflow: document.documentElement.scrollWidth - innerWidth})'));
    check('width ' + width + 'px: no horizontal overflow', layout.overflow <= 1, layout);
    check('width ' + width + 'px: tabs fit', layout.tabsScroll <= layout.tabsClient + 1,
      { scroll: layout.tabsScroll, client: layout.tabsClient });
  }
  /* RU labels at the smallest width as well */
  await setViewport(320, 720);
  await evaluate('chooseLocale("ru")');
  await sleep(500);
  const ruSmall = JSON.parse(await evaluate('JSON.stringify({overflow: document.documentElement.scrollWidth - innerWidth,' +
    'label: document.getElementById("tab-bars").textContent})'));
  check('320px with Russian labels: no overflow', ruSmall.overflow <= 1, ruSmall);

  console.log('');
  console.log('device QA checks passed: ' + passed + ', failed: ' + failures.length);
  if (failures.length) { console.log('FAILURES: ' + failures.join(' | ')); }
  console.log(failures.length === 0 ? 'ALL DEVICE QA CHECKS PASSED' : 'DEVICE QA FAILED');
  browser.kill();
  process.exit(failures.length === 0 ? 0 : 1);
})().catch((error) => { console.error('device QA crashed: ' + (error && error.stack ? error.stack : error)); process.exit(2); });
