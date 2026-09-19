'use strict';
/*
 * Headless UI tests for web/index.html — real Chrome, CDP, no npm dependencies.
 *
 * A tiny local server serves the REAL web/index.html and a controllable
 * /api/weather stub, so every data state can be forced. Chrome runs headless
 * with remote debugging; the tests drive real DOM clicks, real keyboard events,
 * real layout measurements and real screenshots.
 *
 * Usage: node ui_test.js <project-dir> [--screenshots]
 */
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execFileSync } = require('child_process');

const PROJECT = process.argv[2] || process.cwd();
const SHOTS = process.argv.includes('--screenshots');
const PORT = 8099;
const CDP_PORT = 9333;
const PROFILE = path.join(require('os').tmpdir(), 'wstest', 'chrome-profile');

/* ----------------------------------------------------------- test payloads */
const FULL = {
  temperature_c: 23.4, humidity_pct: 48, pressure_hpa: 1012.6, pressure_pa: 101260,
  bmp280_valid: true, dht11_valid: true, bmp280_age_ms: 1200, dht11_age_ms: 800, uptime_ms: 123456
};
const HOT = {
  temperature_c: 31.2, humidity_pct: 12, pressure_hpa: 1031.4, pressure_pa: 103140,
  bmp280_valid: true, dht11_valid: true, bmp280_age_ms: 300, dht11_age_ms: 200, uptime_ms: 5000
};
const NULLS = {
  temperature_c: null, humidity_pct: null, pressure_hpa: null, pressure_pa: null,
  bmp280_valid: false, dht11_valid: false, bmp280_age_ms: null, dht11_age_ms: null, uptime_ms: 900
};
const STALE = {
  temperature_c: 21.5, humidity_pct: 55, pressure_hpa: 1005.5, pressure_pa: 100550,
  bmp280_valid: false, dht11_valid: false, bmp280_age_ms: 45000, dht11_age_ms: 45000, uptime_ms: 60000
};

const api = { mode: 'json', payload: FULL };

/* -------------------------------------------------------------- test server */
let apiHits = 0;
const apiUrls = [];
const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  if (url === '/api/weather') {
    apiHits += 1;
    apiUrls.push(req.url);
    if (api.mode === 'http500') { res.writeHead(500, { 'Content-Type': 'application/json' }); res.end('{}'); return; }
    if (api.mode === 'badjson') { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end('<html>nope'); return; }
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(api.payload));
    return;
  }
  const file = path.join(PROJECT, 'web', 'index.html');
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(fs.readFileSync(file));
});

/* ------------------------------------------------------------- CDP client */
function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(wsUrl);
    let nextId = 1;
    const pending = new Map();
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
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
      close() { socket.close(); }
    }));
  });
}

async function fetchJson(url) { return (await fetch(url)).json(); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ------------------------------------------------------------ assertions */
let passed = 0;
const failures = [];
function check(name, condition, detail) {
  if (condition) { passed += 1; return true; }
  failures.push(name + (detail === undefined ? '' : ' — ' + JSON.stringify(detail)));
  return false;
}
function near(a, b, tolerance) { return Math.abs(a - b) <= tolerance; }

(async function main() {
  await new Promise((resolve) => server.listen(PORT, '127.0.0.1', resolve));

  const chrome = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
  fs.mkdirSync(PROFILE, { recursive: true });
  const browser = spawn(chrome, [
    '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--disable-background-networking', '--mute-audio',
    '--remote-debugging-port=' + CDP_PORT, '--user-data-dir=' + PROFILE, 'about:blank'
  ], { stdio: 'ignore' });

  let target = null;
  for (let attempt = 0; attempt < 60 && !target; attempt += 1) {
    await sleep(250);
    try {
      const list = await fetchJson('http://127.0.0.1:' + CDP_PORT + '/json/list');
      target = list.find((entry) => entry.type === 'page');
    } catch (error) { /* not ready */ }
  }
  if (!target) { throw new Error('Chrome DevTools endpoint did not come up'); }

  const cdp = await connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');

  await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
    source: 'window.__fetchLog = []; const __origFetch = window.fetch;' +
      'window.fetch = function () { window.__fetchLog.push(Array.prototype.slice.call(arguments)[0]);' +
      'return __origFetch.apply(this, arguments); };'
  });

  const evaluate = async (expression) => {
    const result = await cdp.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (result.exceptionDetails) {
      throw new Error('page eval failed: ' +
        JSON.stringify(result.exceptionDetails.exception || result.exceptionDetails));
    }
    return result.result.value;
  };
  const setViewport = (width, height) => cdp.send('Emulation.setDeviceMetricsOverride', {
    width, height: height || 900, deviceScaleFactor: 1, mobile: width < 768
  });

  await setViewport(1024, 900);
  await cdp.send('Page.navigate', { url: 'http://127.0.0.1:' + PORT + '/web/index.html' });
  for (let attempt = 0; attempt < 60; attempt += 1) {
    await sleep(200);
    try { if (await evaluate('document.readyState === "complete" && typeof uiState !== "undefined"')) { break; } }
    catch (error) { /* loading */ }
  }
  await sleep(700);

  /* ------------------------------------------------- language selection (RU/EN) */
  apiHits = 0;
  apiUrls.length = 0;
  check('language screen is the first screen',
    await evaluate('!document.getElementById("language-screen").hidden') === true);
  check('dashboard is hidden before a language is chosen',
    await evaluate('document.getElementById("app-shell").hidden') === true);
  check('locale is unset on start', await evaluate('uiState.locale') === null);
  check('screen state is language-selection',
    await evaluate('uiState.screen') === 'language-selection');
  check('dashboard is not built yet (no polling)',
    await evaluate('uiState.dashboardReady') === false);
  check('two language buttons with locale ids ru/en',
    await evaluate('Array.from(document.querySelectorAll(".langbutton[data-locale]")).map(b => b.dataset.locale).join()') === 'ru,en');
  check('language screen shows both native names',
    await evaluate('document.getElementById("lang-ru").textContent.indexOf("РУССКИЙ") >= 0 && document.getElementById("lang-en").textContent.indexOf("ENGLISH") >= 0'));
  await sleep(400);
  check('choosing a language performs no /api/weather request', apiHits === 0, 'apiHits=' + apiHits);

  /* click РУССКИЙ: the dashboard appears in Russian */
  await evaluate('document.getElementById("lang-ru").click()');
  await sleep(400);
  check('clicking РУССКИЙ sets locale ru', await evaluate('uiState.locale') === 'ru');
  check('dashboard is visible after the choice',
    await evaluate('document.getElementById("app-shell").hidden') === false);
  check('language screen is hidden after the choice',
    await evaluate('document.getElementById("language-screen").hidden') === true);
  check('tabs are Russian after choosing ru',
    await evaluate('document.getElementById("tab-horizontal").textContent') === 'Горизонтальные');
  check('reference tag of the language choice is gone from the DOM text',
    await evaluate('document.body.innerText.indexOf("Выберите язык")') === -1);
  /* After the choice the dashboard polls normally — with the UNCHANGED endpoint:
     the language never reaches the API (it is presentation-layer state). */
  check('the language is never sent to the API',
    apiUrls.every(u => u === '/api/weather') && apiUrls.length > 0,
    JSON.stringify(apiUrls.slice(0, 3)));
  apiHits = 0;
  apiUrls.length = 0;

  /* ---------------------------------------------------------------- structure */
  check('tablist present', await evaluate('!!document.querySelector(\'[role="tablist"]\')'));
  check('5 tabs present', await evaluate('document.querySelectorAll(\'[role="tab"]\').length') === 5);
  check('5 tabpanels present', await evaluate('document.querySelectorAll(\'[role="tabpanel"]\').length') === 5);
  check('mode ids numeric/horizontal/bars/gauges/history',
    await evaluate('Array.from(document.querySelectorAll(\'[role="tab"]\')).map(t => t.dataset.mode).join()') ===
    'numeric,horizontal,bars,gauges,history');
  check('tabpanels labelled by tabs',
    await evaluate('Array.from(document.querySelectorAll(\'[role="tabpanel"]\')).every(p => document.getElementById(p.getAttribute("aria-labelledby")))'));
  check('default mode is numeric', await evaluate('uiState.activeMode') === 'numeric');
  check('only numeric panel visible',
    await evaluate('Array.from(document.querySelectorAll(\'[role="tabpanel"]\')).filter(p => !p.hidden).map(p => p.dataset.mode).join()') === 'numeric');
  check('no external resources',
    await evaluate('performance.getEntriesByType("resource").filter(e => !e.name.startsWith(location.origin)).length') === 0);
  check('no external script/link tags',
    await evaluate('document.querySelectorAll(\'script[src], link[href^="http"]\').length') === 0);

  /* -------------------------------------------------------- numeric rendering */
  check('numeric temperature value', await evaluate('document.querySelector(\'#panel-numeric [data-metric="temperature"] [data-role="value"]\').textContent') === '23.4');
  check('numeric humidity value', await evaluate('document.querySelector(\'#panel-numeric [data-metric="humidity"] [data-role="value"]\').textContent') === '48');
  check('numeric pressure value', await evaluate('document.querySelector(\'#panel-numeric [data-metric="pressure"] [data-role="value"]\').textContent') === '1012.6');
  check('numeric zones: temp-comfort / humidity-comfort / pressure-normal',
    await evaluate('["temperature","humidity","pressure"].map(k => document.querySelector(`#panel-numeric [data-metric="${k}"]`).dataset.tone).join()') === 'temp-comfort,humidity-comfort,pressure-normal');
  check('numeric badges FRESH',
    await evaluate('Array.from(document.querySelectorAll(\'#panel-numeric [data-role="badge"]\')).map(b => b.textContent).join()') === 'FRESH,FRESH,FRESH');

  /* ----------------------------------------------------- horizontal geometry */
  await evaluate('setActiveMode("horizontal")');
  const fill = await evaluate('parseFloat(document.querySelector(\'#panel-horizontal [data-metric="temperature"] [data-role="fill"]\').style.width)');
  check('horizontal temperature fill ≈ 66.8 %', near(fill, 66.8, 0.2), fill);
  const knob = await evaluate('parseFloat(document.querySelector(\'#panel-horizontal [data-metric="temperature"] [data-role="knob"]\').style.left)');
  check('horizontal knob matches fill', near(knob, fill, 0.01), { fill, knob });
  check('horizontal shows numeric value too',
    await evaluate('document.querySelector(\'#panel-horizontal [data-metric="humidity"] [data-role="value"]\').textContent') === '48');
  check('horizontal ticks built from config',
    await evaluate('document.querySelectorAll(\'#panel-horizontal [data-metric="temperature"] .track__tick\').length') === 6);

  /* ------------------------------------------------------------ bars geometry */
  await evaluate('setActiveMode("bars")');
  const barHeight = await evaluate('parseFloat(document.querySelector(\'#panel-bars [data-metric="temperature"] [data-role="bar"]\').style.height)');
  check('bars temperature height ≈ 66.8 %', near(barHeight, 66.8, 0.2), barHeight);
  check('bars show axis range labels',
    await evaluate('Array.from(document.querySelectorAll(\'#panel-bars [data-metric="pressure"] .bcol__axis span\')).map(s => s.textContent).join("|")') === '1050|1000|950');
  check('bars columns = 3', await evaluate('document.querySelectorAll(\'#panel-bars .bcol\').length') === 3);

  /* ---------------------------------------------------------- gauges geometry */
  await evaluate('setActiveMode("gauges")');
  const expectedArc = 2 * Math.PI * 52 * 0.75;
  const dash = await evaluate('document.querySelector(\'#panel-gauges [data-metric="temperature"] [data-role="progress"]\').getAttribute("stroke-dasharray")');
  check('gauge progress matches fraction', near(parseFloat(dash.split(' ')[0]), 0.668 * expectedArc, 0.6), dash);
  check('gauge centre value & unit',
    await evaluate('document.querySelector(\'#panel-gauges [data-metric="humidity"] [data-role="value"]\').textContent') === '48');
  check('gauges count = 3', await evaluate('document.querySelectorAll(\'#panel-gauges .gauge\').length') === 3);

  /* ------------------------------------- tab switching must not fetch or poll */
  await evaluate('clearTimeout(uiState.timerId); uiState.timerId = null; APP_CONFIG.pollIntervalMs = 600000;');
  const fetchesBeforeSwitch = await evaluate('window.__fetchLog.length');
  const pollsBeforeSwitch = await evaluate('uiState.pollCount');
  for (const mode of ['numeric', 'horizontal', 'bars', 'gauges', 'history', 'numeric']) {
    await evaluate('document.getElementById("tab-' + mode + '").click()');
    await sleep(90);
  }
  await sleep(200);
  check('tab switch performs no fetch', await evaluate('window.__fetchLog.length') === fetchesBeforeSwitch,
    { before: fetchesBeforeSwitch, after: await evaluate('window.__fetchLog.length') });
  check('tab switch does not restart polling', await evaluate('uiState.pollCount') === pollsBeforeSwitch);
  check('active tab aria-selected is exclusive',
    await evaluate('Array.from(document.querySelectorAll(\'[role="tab"]\')).filter(t => t.getAttribute("aria-selected") === "true").length') === 1);
  check('roving tabindex correct',
    await evaluate('Array.from(document.querySelectorAll(\'[role="tab"]\')).filter(t => t.tabIndex === 0).length') === 1);

  /* ---------------------------------------- keyboard tab activation (5 modes) */
  await evaluate('document.getElementById("tab-numeric").focus()');
  const pressKey = async (key, code, vk) => {
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key, code, windowsVirtualKeyCode: vk });
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk });
    await sleep(80);
  };
  await pressKey('ArrowRight', 'ArrowRight', 39);
  check('ArrowRight moves to horizontal', await evaluate('uiState.activeMode') === 'horizontal');
  await pressKey('End', 'End', 35);
  check('End key reaches the last mode (history)', await evaluate('uiState.activeMode') === 'history');
  check('focus follows the selected tab', await evaluate('document.activeElement.dataset.mode') === 'history');
  await pressKey('Home', 'Home', 36);
  check('Home key returns to numeric', await evaluate('uiState.activeMode') === 'numeric');

  /* =====================================================================
     HISTORY
     ===================================================================== */
  const FULL_MODEL = 'normalizeWeather(' + JSON.stringify(FULL) + ')';

  /* ---------------------------------------------------------- empty state */
  await evaluate('resetHistory(); setHistoryRetention("10s"); setActiveMode("history")');
  const emptyState = await evaluate(`(() => ({
    count: history.count,
    emptyVisible: !document.querySelector('#panel-history [data-role="empty"]').hidden,
    chartHidden: document.querySelector('#panel-history [data-role="chart"]').hidden,
    text: document.querySelector('#panel-history [data-role="empty"]').textContent.replace(/\\s+/g, ' ').trim(),
    tableRows: document.querySelectorAll('#panel-history [data-role="table-body"] tr').length
  }))()`);
  check('history empty state visible on a fresh page', emptyState.emptyVisible && emptyState.count === 0, emptyState);
  check('empty state explains what is happening',
    /Собираем историю/.test(emptyState.text) && /Первые точки появятся/.test(emptyState.text), emptyState.text);
  check('no fake data in the empty state', emptyState.tableRows === 0 && emptyState.chartHidden);

  /* ------------------------------------------------- sampling + chronology */
  const sampled = await evaluate(`(() => {
    resetHistory();
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 8; i += 1) { updateHistory(model, now - 7000 + i * 1000); }
    const times = [];
    for (let i = 0; i < history.count; i += 1) { times.push(historySampleAt(i).time); }
    return { count: history.count, times: times, capacity: history.capacity, total: history.totalSamples };
  })()`);
  check('10s preset stores every second of polling', sampled.count === 8, sampled);
  check('samples are chronological and strictly increasing',
    sampled.times.every((t, i) => i === 0 || t > sampled.times[i - 1]), sampled.times);
  check('capacity matches the preset maxPoints', sampled.capacity === 14, sampled.capacity);
  check('cumulative counter tracks samples', sampled.total === 8, sampled.total);

  const duplicate = await evaluate(`(() => {
    const model = ${FULL_MODEL};
    const before = history.count;
    const last = historySampleAt(history.count - 1).time;
    updateHistory(model, last);
    const afterSame = history.count;
    updateHistory(model, last + 10);
    return { before: before, afterSame: afterSame, afterNear: history.count };
  })()`);
  check('duplicate timestamp is not stored twice',
    duplicate.afterSame === duplicate.before && duplicate.afterNear === duplicate.before, duplicate);

  const adaptive = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('24h');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 12; i += 1) { updateHistory(model, now - 18000 + i * 1500); }
    const earlyCount = history.count;
    const base = now;
    for (let i = 0; i < 40; i += 1) { updateHistory(model, base + 300000 * (i + 1)); }
    const afterLong = history.count;
    setHistoryRetention('10s');
    return { earlyCount: earlyCount, afterLong: afterLong };
  })()`);
  check('24h preset keeps a single point from a short burst', adaptive.earlyCount === 1, adaptive);
  check('24h sampling takes about one point per 5 minutes', adaptive.afterLong === 41, adaptive);

  /* ------------------------------------------------------ trimming + bounds */
  const trimmed = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 40; i += 1) { updateHistory(model, now - 39000 + i * 1000); }
    const oldest = historySampleAt(0).time;
    return { count: history.count, age: now - oldest, capacity: history.capacity };
  })()`);
  check('trimming drops samples outside the retention window',
    trimmed.age <= 10000 && trimmed.count <= 11, trimmed);
  check('ring never exceeds capacity', trimmed.count <= trimmed.capacity, trimmed);

  const bounded = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('30s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 400; i += 1) { updateHistory(model, now - 399000 + i * 1000); }
    return { count: history.count, capacity: history.capacity, hard: HISTORY_CONFIG.hardMaxPoints };
  })()`);
  check('maxPoints bounds the buffer under a long burst',
    bounded.count <= bounded.capacity && bounded.count <= bounded.hard && bounded.count > 0, bounded);

  /* ------------------------------------------------------------- null data */
  const nullCase = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const now = Date.now();
    const model = normalizeWeather(${JSON.stringify(NULLS)});
    updateHistory(model, now);
    const sample = historySampleAt(0);
    const series = buildHistorySeries(now, { tMin: now - 1000, tMax: now });
    return {
      rawTemperature: String(sample.temperature),
      isNull: historyMetricValue(sample, 'temperature') === null,
      state: historyMetricState(sample, 'temperature'),
      drawnPoints: series.metrics.temperature.drawn.length
    };
  })()`);
  check('null is stored as missing, never as 0',
    nullCase.isNull && nullCase.rawTemperature === 'NaN', nullCase);
  check('null sample records ERROR state', nullCase.state === 'ERROR', nullCase.state);
  check('null produces no drawn point (a real gap)', nullCase.drawnPoints === 0, nullCase);

  const gapCase = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('30s');
    const good = ${FULL_MODEL};
    const bad = normalizeWeather(${JSON.stringify(NULLS)});
    const now = Date.now();
    updateHistory(good, now - 4000);
    updateHistory(bad, now - 2000);
    updateHistory(good, now);
    const series = buildHistorySeries(now, { tMin: now - 4000, tMax: now });
    const segments = new Set(series.metrics.temperature.points.filter(p => p.value !== null).map(p => p.segment));
    return { count: history.count, segments: segments.size };
  })()`);
  check('a missing sample splits the line into separate segments', gapCase.segments === 2, gapCase);

  /* -------------------------------------------------------- stale / invalid */
  const staleCase = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    updateHistory(normalizeWeather(${JSON.stringify(STALE)}), Date.now());
    const sample = historySampleAt(0);
    return {
      state: historyMetricState(sample, 'temperature'),
      value: historyMetricValue(sample, 'temperature'),
      humidityState: historyMetricState(sample, 'humidity')
    };
  })()`);
  check('valid=false is stored as STALE and keeps the last known value',
    staleCase.state === 'STALE' && staleCase.value === 21.5, staleCase);
  check('per-metric states are independent in history', staleCase.humidityState === 'STALE', staleCase);

  /* -------------------------------------------------- retention switching */
  const retentionSwitch = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 8; i += 1) { updateHistory(model, now - 7000 + i * 1000); }
    const before = history.count;
    setHistoryRetention('24h');
    const after = history.count;
    const capacity = history.capacity;
    setHistoryRetention('30s');
    const thinned = history.count;
    return { before: before, after: after, capacity: capacity, thinned: thinned, preset: uiState.historyPresetId };
  })()`);
  check('retention change keeps the same single store',
    retentionSwitch.after >= 1 && retentionSwitch.after <= retentionSwitch.before, retentionSwitch);
  check('retention change re-dimensions the buffer to 24h maxPoints',
    retentionSwitch.capacity === 292, retentionSwitch.capacity);
  check('switching to a coarser preset re-thins the trail',
    retentionSwitch.thinned >= 1 && retentionSwitch.thinned <= 2, retentionSwitch);
  check('selected preset is applied', retentionSwitch.preset === '30s');

  /* ------------------------------------------------------ clear + refill */
  const clearFlow = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 5; i += 1) { updateHistory(model, now - 4000 + i * 1000); }
    setActiveMode('history');
    const button = document.querySelector('#panel-history [data-role="clear"]');
    const before = history.count;
    button.click();
    const armed = button.dataset.armed === 'true';
    const afterArm = history.count;
    button.click();
    const afterClear = history.count;
    const emptyVisible = !document.querySelector('#panel-history [data-role="empty"]').hidden;
    for (let i = 0; i < 3; i += 1) { updateHistory(model, now - 2000 + i * 1000); }
    return { before: before, armed: armed, afterArm: afterArm, afterClear: afterClear,
             emptyVisible: emptyVisible, refilled: history.count, buttonText: button.textContent };
  })()`);
  check('clear needs two steps (no accidental wipe)',
    clearFlow.armed && clearFlow.afterArm === clearFlow.before, clearFlow);
  check('clear empties the ring and shows the empty state',
    clearFlow.afterClear === 0 && clearFlow.emptyVisible, clearFlow);
  check('history refills after clearing', clearFlow.refilled === 3, clearFlow);
  check('clear button label returns to normal', clearFlow.buttonText === 'Очистить историю', clearFlow.buttonText);

  /* ------------------------------------------------------------- the chart */
  const chart = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('5m');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 30; i += 1) { updateHistory(model, now - 145000 + i * 5000); }
    setActiveMode('numeric');
    setActiveMode('history');
    const series = buildHistorySeries(now);
    const lanes = {};
    ['temperature', 'humidity', 'pressure'].forEach((key) => {
      const lane = series.metrics[key];
      lanes[key] = { yMin: lane.yMin, yMax: lane.yMax, drawn: lane.drawn.length };
    });
    return {
      count: history.count,
      paths: Array.from(document.querySelectorAll('#panel-history .chart__line')).map(p => ({
        metric: p.dataset.metric, length: (p.getAttribute('d') || '').length, state: p.dataset.state
      })),
      lanes: lanes,
      xLabels: Array.from(document.querySelectorAll('#panel-history .chart__xlist')).map(t => t.textContent),
      yLabels: Array.from(document.querySelectorAll('#panel-history .chart__ylist')).map(t => t.textContent),
      ariaLabel: document.querySelector('#panel-history [data-role="chart-svg"]').getAttribute('aria-label'),
      emptyHidden: document.querySelector('#panel-history [data-role="empty"]').hidden,
      emptyDisplay: getComputedStyle(document.querySelector('#panel-history [data-role="empty"]')).display,
      chartDisplay: getComputedStyle(document.querySelector('#panel-history [data-role="chart"]')).display
    };
  })()`);
  check('history renders three metric lines',
    chart.paths.length === 3 && chart.paths.every(p => p.length > 10), chart.paths);
  check('lines carry metric identity and a data state',
    chart.paths.map(p => p.metric).join() === 'temperature,humidity,pressure' &&
    chart.paths.every(p => ['FRESH', 'STALE', 'ERROR'].indexOf(p.state) >= 0), chart.paths);
  check('humidity uses its own fixed 0..100 scale',
    chart.lanes.humidity.yMin === 0 && chart.lanes.humidity.yMax === 100, chart.lanes.humidity);
  check('temperature and pressure each get a dynamic scale',
    chart.lanes.temperature.yMin !== chart.lanes.humidity.yMin &&
    chart.lanes.pressure.yMin !== chart.lanes.temperature.yMin, chart.lanes);
  check('pressure scale keeps a minimum span (no false drama)',
    (chart.lanes.pressure.yMax - chart.lanes.pressure.yMin) >= 4, chart.lanes.pressure);
  check('every lane has labelled Y axis ticks',
    chart.yLabels.filter(t => t !== '').length === 9, chart.yLabels);
  check('X axis shows 5 time ticks', chart.xLabels.length === 5 && chart.xLabels.every(t => /^\d\d:\d\d/.test(t)),
    chart.xLabels);
  check('chart is described for assistive tech', /История измерений/.test(chart.ariaLabel), chart.ariaLabel);
  check('chart replaces the empty state once data exists (computed, not just the attribute)',
    chart.emptyHidden === true && chart.emptyDisplay === 'none' && chart.chartDisplay !== 'none', chart);

  /* --------------------------------------------- downsampling for rendering */
  const downsample = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('24h');
    const now = Date.now();
    for (let i = 0; i < 289; i += 1) {
      const scaled = JSON.parse(JSON.stringify(${JSON.stringify(FULL)}));
      if (i === 150) { scaled.temperature_c = 39.5; }
      if (i === 151) { scaled.temperature_c = -5.5; }
      updateHistory(normalizeWeather(scaled), now - 288 * 300000 + i * 300000);
    }
    const series = buildHistorySeries(now);
    const drawn = series.metrics.temperature.drawn;
    return {
      samples: history.count,
      drawn: drawn.length,
      limit: HISTORY_CONFIG.maxRenderPoints,
      min: Math.min.apply(null, drawn.map(p => p.value)),
      max: Math.max.apply(null, drawn.map(p => p.value)),
      svgNodes: document.querySelectorAll('#panel-history .chart__svg *').length
    };
  })()`);
  check('rendering uses a bounded number of visual points',
    downsample.drawn <= downsample.limit && downsample.drawn > 0, downsample);
  check('downsampling preserves spikes', downsample.max === 39.5 && downsample.min === -5.5, downsample);
  check('raw samples are far more numerous than drawn points',
    downsample.samples > downsample.drawn * 1.4, downsample);
  check('SVG node count stays small regardless of samples', downsample.svgNodes < 120, downsample.svgNodes);

  /* ---------------------------------------------------------- tooltip + keys */
  const tooltip = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 5; i += 1) { updateHistory(model, now - 4000 + i * 1000); }
    setActiveMode('history');
    uiState.hoverIndex = 3;
    renderHistoryTooltip();
    const tip = document.querySelector('#panel-history [data-role="tooltip"]');
    return {
      hidden: tip.hidden,
      time: document.querySelector('#panel-history [data-role="tt-time"]').textContent,
      expectedTime: formatClock(historySampleAt(3).time, true),
      temperature: document.querySelector('#panel-history [data-role="tt-value-temperature"]').textContent,
      pressure: document.querySelector('#panel-history [data-role="tt-value-pressure"]').textContent,
      state: document.querySelector('#panel-history [data-role="tt-state-temperature"]').textContent,
      cursorVisible: document.querySelector('#panel-history [data-role="cursor"]').style.display !== 'none'
    };
  })()`);
  check('tooltip shows the selected sample',
    tooltip.hidden === false && tooltip.time === tooltip.expectedTime &&
    tooltip.temperature === '23.4' && tooltip.pressure === '1012.6', tooltip);
  check('tooltip shows the data state', tooltip.state === 'FRESH', tooltip.state);
  check('cursor guide line appears with the tooltip', tooltip.cursorVisible);

  await evaluate('document.querySelector(\'#panel-history [data-role="chart"]\').focus()');
  await pressKey('ArrowRight', 'ArrowRight', 39);
  const keyed = await evaluate('({ index: uiState.hoverIndex, time: document.querySelector(\'#panel-history [data-role="tt-time"]\').textContent })');
  check('keyboard moves the tooltip cursor', keyed.index === 4 && /\d\d:\d\d:\d\d/.test(keyed.time), keyed);
  await pressKey('Escape', 'Escape', 27);
  check('Escape hides the tooltip cursor',
    await evaluate('uiState.hoverIndex') === -1 &&
    await evaluate('document.querySelector(\'#panel-history [data-role="tooltip"]\').hidden') === true);

  const tableRows = await evaluate('document.querySelectorAll(\'#panel-history [data-role="table-body"] tr\').length');
  check('text/table alternative is available for screen readers', tableRows === 5, tableRows);

  /* ------------------------------------------- no second fetch, no DOM churn */
  await evaluate('setActiveMode("history"); APP_CONFIG.pollIntervalMs = 1200; scheduleNextPoll();');
  const fetchBeforeHistory = await evaluate('window.__fetchLog.length');
  await sleep(3600);
  const fetchAfterHistory = await evaluate('window.__fetchLog.length');
  check('history adds no extra fetch loop', fetchAfterHistory - fetchBeforeHistory <= 4 && fetchAfterHistory > fetchBeforeHistory,
    { before: fetchBeforeHistory, after: fetchAfterHistory });
  check('polling continues while history is open', await evaluate('uiState.activeMode') === 'history');
  await evaluate('clearTimeout(uiState.timerId); uiState.timerId = null; APP_CONFIG.pollIntervalMs = 600000;');

  const domStability = await evaluate(`(() => {
    const before = document.querySelectorAll('*').length;
    renderHistory(uiState.uiModel);
    renderHistory(uiState.uiModel);
    renderHistory(uiState.uiModel);
    return { before: before, after: document.querySelectorAll('*').length };
  })()`);
  check('re-rendering the chart does not grow the DOM',
    domStability.after === domStability.before, domStability);
  check('history listeners are attached once (no accumulation)',
    await evaluate('document.querySelectorAll(\'#panel-history [data-preset]\').length') === 11);

  /* ------------------------------------------------------------ animations */
  await cdp.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: 'reduce' }] });
  const reduced = await evaluate(`(() => {
    const node = document.querySelector('#panel-numeric [data-metric="temperature"] [data-role="value"]');
    node.__number = 10; node.__target = '10.0'; node.textContent = '10.0';
    setMetricValue(node, '25.0', 25, 1);
    const panel = document.querySelector('#panel-numeric');
    return {
      reduced: prefersReducedMotion(),
      tweens: tweens.size,
      text: node.textContent,
      animationDuration: getComputedStyle(panel).animationDuration
    };
  })()`);
  check('prefers-reduced-motion disables number tweening',
    reduced.reduced === true && reduced.tweens === 0 && reduced.text === '25.0', reduced);
  check('panel animation is effectively disabled with reduced motion',
    parseFloat(reduced.animationDuration) <= 0.001, reduced.animationDuration);

  await cdp.send('Emulation.setEmulatedMedia', { features: [] });
  const tweenStart = await evaluate(`(() => {
    const node = document.querySelector('#panel-numeric [data-metric="humidity"] [data-role="value"]');
    node.__number = 10; node.__target = '10'; node.textContent = '10';
    setMetricValue(node, '60', 60, 0);
    return { tweens: tweens.size, running: tweenRafId !== null };
  })()`);
  check('motion enabled: numbers tween instead of jumping',
    tweenStart.tweens === 1 && tweenStart.running === true, tweenStart);
  await sleep(430);
  const tweenEnd = await evaluate(`(() => {
    const node = document.querySelector('#panel-numeric [data-metric="humidity"] [data-role="value"]');
    return { tweens: tweens.size, raf: tweenRafId, text: node.textContent };
  })()`);
  check('tween settles exactly and stops (no endless loop)',
    tweenEnd.tweens === 0 && tweenEnd.raf === null && tweenEnd.text === '60', tweenEnd);

  /* ------------------------------------------------------------- retention UI */
  const retentionUi = await evaluate(`(() => {
    setActiveMode('history');
    const select = document.querySelector('#panel-history [data-role="retention-select"]');
    const chips = Array.from(document.querySelectorAll('#panel-history [data-preset]'));
    select.value = '1h';
    select.dispatchEvent(new Event('change', { bubbles: true }));
    const afterSelect = { preset: uiState.historyPresetId,
      pressed: chips.filter(c => c.getAttribute('aria-pressed') === 'true').map(c => c.dataset.preset) };
    chips.filter(c => c.dataset.preset === '10s')[0].click();
    return {
      optionCount: select.options.length,
      values: Array.from(select.options).map(o => o.value),
      chips: chips.length,
      afterSelect: afterSelect,
      afterChip: { preset: uiState.historyPresetId, selectValue: select.value },
      minRetention: HISTORY_CONFIG.minRetentionMs,
      maxRetention: HISTORY_CONFIG.maxRetentionMs
    };
  })()`);
  check('retention control offers 11 presets from 10 s to 24 h',
    retentionUi.optionCount === 11 && retentionUi.chips === 11 &&
    retentionUi.values[0] === '10s' && retentionUi.values[10] === '24h' &&
    retentionUi.minRetention === 10000 && retentionUi.maxRetention === 86400000, retentionUi);
  check('select changes retention and syncs the chips',
    retentionUi.afterSelect.preset === '1h' && retentionUi.afterSelect.pressed.join() === '1h', retentionUi.afterSelect);
  check('chip switches back and syncs the select',
    retentionUi.afterChip.preset === '10s' && retentionUi.afterChip.selectValue === '10s', retentionUi.afterChip);

  /* --------------------------------------------------------- offline history */
  const offlineHistory = await evaluate(`(() => {
    resetHistory();
    setHistoryRetention('10s');
    const model = ${FULL_MODEL};
    const now = Date.now();
    for (let i = 0; i < 6; i += 1) { updateHistory(model, now - 5000 + i * 1000); }
    setActiveMode('history');
    return { count: history.count };
  })()`);
  api.mode = 'http500';
  await evaluate('(async () => { await pollOnce(); })()');
  await sleep(150);
  const offlineKeepsHistory = await evaluate(`(() => ({
    connection: uiState.connection,
    count: history.count,
    paths: Array.from(document.querySelectorAll('#panel-history .chart__line')).filter(p => (p.getAttribute('d') || '').length > 5).length,
    mode: uiState.activeMode
  }))()`);
  check('offline keeps the collected history and the mode',
    offlineKeepsHistory.connection === 'OFFLINE' && offlineKeepsHistory.count === offlineHistory.count &&
    offlineKeepsHistory.paths === 3 && offlineKeepsHistory.mode === 'history', offlineKeepsHistory);
  api.mode = 'json';
  await evaluate('(async () => { await pollOnce(); })()');
  await sleep(150);
  check('recovery restores ONLINE while history stays',
    await evaluate('uiState.connection') === 'ONLINE' && await evaluate('history.count') >= offlineHistory.count);

  /* ------------------------------------------------- switching does not fetch */
  const beforeSwitchAll = await evaluate('window.__fetchLog.length');
  for (const mode of ['history', 'numeric', 'gauges', 'history', 'bars', 'history']) {
    await evaluate('document.getElementById("tab-' + mode + '").click()');
    await sleep(50);
  }
  check('switching through all five modes performs no fetch',
    await evaluate('window.__fetchLog.length') === beforeSwitchAll,
    { before: beforeSwitchAll, after: await evaluate('window.__fetchLog.length') });

  /* --------------------------------------------------------------- responsive */
  const widths = [320, 375, 430, 768, 1024, 1280, 1920];
  const modes = ['numeric', 'horizontal', 'bars', 'gauges', 'history'];
  const overflow = {};
  const geometry = {};
  for (const width of widths) {
    await setViewport(width, 900);
    await sleep(120);
    for (const mode of modes) {
      await evaluate('setActiveMode("' + mode + '")');
      await sleep(70);
      const metrics = await evaluate(`(() => {
        const de = document.documentElement;
        let maxRight = 0;
        document.querySelectorAll('body *').forEach((el) => {
          // Skip the visually-hidden screen-reader table: it is clipped by design.
          if (el.closest('.visually-hidden')) { return; }
          const rect = el.getBoundingClientRect();
          if (rect.width > 0 && rect.height > 0) { maxRight = Math.max(maxRight, rect.right); }
        });
        const chart = document.querySelector('#panel-history [data-role="chart"]');
        const axisLabel = document.querySelector('#panel-history .chart__xlist');
        return {
          scrollWidth: de.scrollWidth, innerWidth: window.innerWidth, bodyScroll: document.body.scrollWidth,
          maxRight: Math.round(maxRight),
          chartWidth: Math.round(chart.getBoundingClientRect().width),
          axisFont: axisLabel ? parseFloat(getComputedStyle(axisLabel).fontSize) : 0,
          tabHeight: Math.round(document.getElementById('tab-history').getBoundingClientRect().height),
          chipHeight: Math.round(document.querySelector('#panel-history [data-preset]').getBoundingClientRect().height)
        };
      })()`);
      const ok = metrics.scrollWidth <= metrics.innerWidth + 1 &&
                 metrics.bodyScroll <= metrics.innerWidth + 1 &&
                 metrics.maxRight <= metrics.innerWidth + 1;
      if (!ok) { overflow[width + 'px/' + mode] = metrics; }
      geometry[width + 'px/' + mode] = metrics;
    }
  }
  check('no horizontal overflow on any width/mode', Object.keys(overflow).length === 0, overflow);
  check('chart width tracks the viewport',
    geometry['320px/history'].chartWidth > 150 && geometry['1920px/history'].chartWidth > 700,
    { small: geometry['320px/history'].chartWidth, large: geometry['1920px/history'].chartWidth });
  check('chart does not stretch beyond a readable width at 1920px',
    geometry['1920px/history'].chartWidth <= 1160, geometry['1920px/history'].chartWidth);
  check('axis labels stay legible (>= 10px)',
    widths.every((w) => geometry[w + 'px/history'].axisFont >= 10),
    widths.map((w) => geometry[w + 'px/history'].axisFont).join());
  check('tab touch targets stay >= 30px tall',
    widths.every((w) => geometry[w + 'px/history'].tabHeight >= 30),
    widths.map((w) => geometry[w + 'px/history'].tabHeight).join());
  check('retention chips are tappable (>= 30px)',
    widths.every((w) => geometry[w + 'px/history'].chipHeight >= 30),
    widths.map((w) => geometry[w + 'px/history'].chipHeight).join());

  if (SHOTS) {
    const dir = path.join(require('os').tmpdir(), 'wstest');
    for (const [width, mode] of [[320, 'history'], [375, 'numeric'], [1024, 'history'], [1280, 'gauges']]) {
      await setViewport(width, 900);
      await evaluate('setActiveMode("' + mode + '")');
      await sleep(400);
      const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
      fs.writeFileSync(path.join(dir, 'shot-' + width + '-' + mode + '.png'), Buffer.from(shot.data, 'base64'));
    }
    console.log('screenshots written to ' + dir);
  }

  /* -------------------------------------------------------------------- report */
  /* ---------------------------------------------------- localization coverage */
  /* English dashboard: every visible string must come from the catalog, so no
     Russian text may remain anywhere on the page. */
  await evaluate('chooseLocale("en")');
  await sleep(400);
  check('chooseLocale(en) sets locale en', await evaluate('uiState.locale') === 'en');
  check('tabs are English after choosing en',
    await evaluate('document.getElementById("tab-horizontal").textContent') === 'HORIZONTAL');
  check('metric label is English in the dashboard',
    await evaluate('METRIC_CONFIG.temperature.label') === 'Temperature');
  check('no untranslated Cyrillic text is visible in EN mode',
    await evaluate('!/[А-Яа-яЁё]/.test(document.body.innerText)'),
    await evaluate('(document.body.innerText.match(/[А-Яа-яЁё]+/g) || []).slice(0, 5).join(",")'));
  check('en dashboards keep the localized retention labels',
    await evaluate('HISTORY_CONFIG.presets.filter(p => !p.label).length') === 0);
  check('CSS no-data placeholder follows the locale',
    await evaluate('getComputedStyle(document.documentElement).getPropertyValue("--nodata-text").indexOf("no data") >= 0'));

  /* A reload must ask again: the choice is deliberately not persisted. */
  await evaluate('window.localStorage.clear(); window.sessionStorage.clear();');
  await cdp.send('Page.reload');
  for (let attempt = 0; attempt < 60; attempt += 1) {
    await sleep(200);
    try { if (await evaluate('document.readyState === "complete" && typeof uiState !== "undefined"')) { break; } }
    catch (error) { /* loading */ }
  }
  await sleep(400);
  check('after reload the language screen is shown again (no persistence)',
    await evaluate('!document.getElementById("language-screen").hidden') === true);
  check('after reload the dashboard is hidden again',
    await evaluate('document.getElementById("app-shell").hidden') === true);
  check('after reload the locale is unset again', await evaluate('uiState.locale') === null);
  check('after reload nothing was persisted for the locale',
    await evaluate('Object.keys(localStorage).filter(k => /locale|lang/i.test(k)).length') === 0);

  /* The English dashboard still works after the reload + a new choice. */
  await evaluate('document.getElementById("lang-en").click()');
  await sleep(400);
  check('English dashboard renders after the reload',
    await evaluate('uiState.locale') === 'en' && await evaluate('document.getElementById("app-shell").hidden') === false);
  check('numeric value rendered after the reload',
    await evaluate('document.querySelector(\'#panel-numeric [data-metric="temperature"] [data-role="value"]\').textContent') === '23.4');

  console.log('checks passed: ' + passed + ', failed: ' + failures.length);
  failures.forEach((failure) => console.log('  FAIL: ' + failure));
  console.log(failures.length ? 'UI TESTS FAILED' : 'ALL UI TESTS PASSED');

  cdp.close();
  server.close();
  try { execFileSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], { stdio: 'ignore' }); } catch (error) { /* gone */ }
  process.exit(failures.length ? 1 : 0);
})().catch((error) => {
  console.error('HARNESS ERROR: ' + (error && error.stack ? error.stack : error));
  try { server.close(); } catch (closeError) { /* ignore */ }
  process.exit(2);
});
