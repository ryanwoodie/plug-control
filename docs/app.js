// Plug Control web app. Talks to the controller through three retained MQTT
// topics per owner, all AES-GCM encrypted with a key derived from the
// owner's password (the broker and this public site never see it):
//   plugctl/<tid>/c  config   (schedules, names)      app -> controller
//   plugctl/<tid>/x  commands (on/off/timer/refresh)   app -> controller
//   plugctl/<tid>/s  status                           controller -> app
'use strict';

const BROKER_WS = 'wss://broker.hivemq.com:8884/mqtt';
const PBKDF2_ITERS = 100000; // must match tools/make_secrets.py
const CMD_MAX_AGE = 900;     // seconds; matches CMD_MAX_AGE_S in firmware
const STALE_AFTER = 10 * 60; // auto-refresh if the last status is older than this
const OFFLINE_AFTER = 6.5 * 3600; // the controller reports at least every 6 h

const TIMEZONES = [
  ['CST6', 'Saskatchewan (CST, no DST)'],
  ['MST7MDT,M3.2.0,M11.1.0', 'Mountain (Alberta)'],
  ['CST6CDT,M3.2.0,M11.1.0', 'Central (Manitoba)'],
  ['PST8PDT,M3.2.0,M11.1.0', 'Pacific (BC)'],
  ['EST5EDT,M3.2.0,M11.1.0', 'Eastern (Ontario, Quebec)'],
  ['AST4ADT,M3.2.0,M11.1.0', 'Atlantic'],
  ['NST3:30NDT,M3.2.0,M11.1.0', 'Newfoundland'],
  ['MST7', 'Arizona (MST, no DST)'],
  ['UTC0', 'UTC'],
];
const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const SRC = { s: 'by schedule', m: 'manually', t: 'by timer', b: 'at the plug', '-': '' };

const enc = new TextEncoder(), dec = new TextDecoder();
const $ = (id) => document.getElementById(id);
const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
const b64 = (u8) => btoa(String.fromCharCode(...u8));
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const nowS = () => Math.floor(Date.now() / 1000);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

let session = null; // {user, key, topics}
let client = null;
let status = null;  // last decrypted status
let config = null;  // last decrypted config (what we believe the controller has)
let cmds = { q: [] };
let pendingCfg = 0;
let gotAnything = false;

// ---- crypto ---------------------------------------------------------------
async function deriveSession(user, pass) {
  user = user.trim().toLowerCase();
  const base = await crypto.subtle.importKey('raw', enc.encode(pass), 'PBKDF2', false, ['deriveBits']);
  const raw = new Uint8Array(await crypto.subtle.deriveBits(
    { name: 'PBKDF2', salt: enc.encode('plugctl1:' + user), iterations: PBKDF2_ITERS, hash: 'SHA-256' }, base, 128));
  return sessionFromRaw(user, raw);
}

async function sessionFromRaw(user, raw) {
  const key = await crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
  const tid = hex(await crypto.subtle.digest('SHA-256', new Uint8Array([...raw, ...enc.encode('topic')]))).slice(0, 20);
  const base = `plugctl/${tid}/`;
  return { user, raw, key, topics: { c: base + 'c', x: base + 'x', s: base + 's' } };
}

async function seal(topic, obj) {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv, additionalData: enc.encode(topic) }, session.key, enc.encode(JSON.stringify(obj))));
  const out = new Uint8Array(12 + ct.length);
  out.set(iv);
  out.set(ct, 12);
  return out;
}

async function open(topic, bytes) {
  const pt = await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv: bytes.slice(0, 12), additionalData: enc.encode(topic) }, session.key, bytes.slice(12));
  return JSON.parse(dec.decode(pt));
}

// ---- MQTT -------------------------------------------------------------------
function connect() {
  client = mqtt.connect(BROKER_WS, {
    clientId: 'pcw-' + hex(crypto.getRandomValues(new Uint8Array(6))),
    reconnectPeriod: 5000,
    connectTimeout: 15000,
  });
  client.on('connect', () => {
    client.subscribe(Object.values(session.topics));
    setBanner(null);
    setTimeout(() => {
      if (!gotAnything) setBanner('No data for this login yet. Check the username and password. If they are right, the controller has not reported in yet.', 'warn');
      else if (!status || nowS() - status.t > STALE_AFTER) refresh();
    }, 6000);
  });
  client.on('offline', () => setBanner('Reconnecting to the message server…', 'warn'));
  client.on('message', async (topic, payload) => {
    let msg;
    try { msg = await open(topic, new Uint8Array(payload)); } catch { return; }
    gotAnything = true;
    if (topic === session.topics.s) { status = msg; if (pendingCfg && msg.cv >= pendingCfg) pendingCfg = 0; }
    else if (topic === session.topics.c) { if (!config || msg.v >= config.v) config = msg; }
    else if (topic === session.topics.x) { cmds = msg; }
    setBanner(null);
    render();
  });
}

async function publish(kind, obj) {
  const topic = session.topics[kind];
  client.publish(topic, await seal(topic, obj), { retain: true, qos: 1 });
}

// ---- actions ----------------------------------------------------------------
async function sendCmd(a, plug, extra = {}) {
  const now = nowS();
  const id = hex(crypto.getRandomValues(new Uint8Array(4)));
  const q = (cmds.q || []).filter((c) => now - c.t < CMD_MAX_AGE && !(status?.ax || []).includes(c.id)).slice(-4);
  q.push({ id, t: now, a, p: plug, ...extra });
  cmds = { q };
  await publish('x', cmds);
  render();
}
const refresh = () => sendCmd('ref', '');

function baseConfig() {
  if (config) return structuredClone(config);
  const p = {};
  for (const [mac, s] of Object.entries(status?.p || {})) p[mac] = { n: s.n, r: [], o: [] };
  return { v: 0, tz: status?.tz || 'CST6', p };
}

async function saveConfig(cfg) {
  cfg.v = Date.now();
  cfg.t = nowS();
  for (const pc of Object.values(cfg.p)) pc.o = (pc.o || []).filter((o) => o.at + o.m * 60 > cfg.t);
  config = cfg;
  pendingCfg = cfg.v;
  await publish('c', cfg);
  render();
}

function plugCfg(cfg, mac) {
  cfg.p[mac] ||= { n: status?.p?.[mac]?.n || mac, r: [], o: [] };
  return cfg.p[mac];
}

// ---- formatting ---------------------------------------------------------------
function fmtDur(m) {
  if (!m) return 'off';
  const h = Math.floor(m / 60), mm = m % 60;
  return h ? `${h} h${mm ? ' ' + mm + ' min' : ''}` : `${mm} min`;
}
function fmtAgo(t) {
  const s = nowS() - t;
  if (s < 60) return 'just now';
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} days ago`;
}
const fmtTime = (t) => new Date(t * 1000).toLocaleString([], { weekday: 'short', hour: 'numeric', minute: '2-digit' });
const fmtHM = (m) => `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
function fmtDays(d) {
  if (d === 127) return 'Every day';
  if (d === 62) return 'Weekdays';
  if (d === 65) return 'Weekends';
  return DAYS.filter((_, i) => d & (1 << i)).join(' ');
}
const kb = (b) => (b >= 1048576 ? (b / 1048576).toFixed(2) + ' MB' : (b / 1024).toFixed(1) + ' KB');
const signal = (csq) => (csq == null || csq === 99 ? 'unknown' : csq >= 20 ? `good (${csq})` : csq >= 10 ? `fair (${csq})` : `weak (${csq})`);

function durOptions(sel, mins, value) {
  sel.innerHTML = mins.map((m) => `<option value="${m}">${fmtDur(m)}</option>`).join('');
  sel.value = String(value);
}
const DURS = [15, 30, 45, 60, 90, 120, 150, 180, 240, 300, 360, 480, 600, 720];

// ---- rendering ------------------------------------------------------------------
function setBanner(text, kind = 'info') {
  const b = $('banner');
  b.hidden = !text;
  b.textContent = text || '';
  b.className = 'banner ' + kind;
}

function pendingFor(mac) {
  const ax = status?.ax || [];
  return (cmds.q || []).filter((c) => c.p === mac && c.a !== 'ref' && !ax.includes(c.id) && nowS() - c.t < CMD_MAX_AGE);
}

function render() {
  const plugsEl = $('plugs');
  const cfg = config || baseConfig();
  const macs = Object.keys(status?.p || cfg.p || {});
  if (!macs.length) {
    plugsEl.innerHTML = '<section class="card muted">Waiting for the controller…</section>';
  } else {
    plugsEl.innerHTML = macs.map((mac) => plugCard(mac, status?.p?.[mac] || {}, cfg.p?.[mac] || { r: [], o: [] })).join('');
  }
  renderDevice();
}

function plugCard(mac, s, pc) {
  const pend = pendingFor(mac);
  const stateCls = !s.ok ? 'unknown' : s.on ? 'on' : 'off';
  // Highlight the button matching the reported state; neither when unknown.
  const isOn = !!(status && s.ok && s.on), isOff = !!(status && s.ok && !s.on);
  const stateTxt = !status ? '—' : !s.ok ? 'Unreachable' : s.on ? 'On' : 'Off';
  const timer = s.on && s.te > nowS() ? `<span class="pill">off at ${fmtTime(s.te)}</span>` : '';
  const via = s.ok && SRC[s.s] ? `<span class="muted small">${s.on ? 'turned on' : 'turned off'} ${SRC[s.s]}</span>` : '';
  const pendTxt = pend.length ? `<p class="pending">Sending “${pend.at(-1).a === 'tmr' ? 'timer' : pend.at(-1).a}” to the controller…</p>` : '';
  const rules = (pc.r || []).map((r, i) => `
      <li class="${r.e === 0 ? 'disabled' : ''}">
        <button class="link" data-act="editRule" data-mac="${mac}" data-i="${i}">
          <b>${esc(r.nm || 'Schedule')}</b> · ${fmtDays(r.d)} · ${fmtHM(r.s)} for ${fmtDur(r.m)}${r.e === 0 ? ' (off)' : ''}
        </button>
      </li>`).join('');
  const ones = (pc.o || []).filter((o) => o.at + o.m * 60 > nowS()).map((o, i) => `
      <li><span>${fmtTime(o.at)} for ${fmtDur(o.m)}</span>
        <button class="link danger" data-act="delOnce" data-mac="${mac}" data-at="${o.at}">Remove</button></li>`).join('');
  return `
  <section class="card plug">
    <div class="row between">
      <h2>${esc(pc.n || s.n || mac)}</h2>
      <button class="link" data-act="plugSettings" data-mac="${mac}">Settings</button>
    </div>
    <div class="state ${stateCls}">
      <span class="dot"></span><span class="big">${stateTxt}</span> ${timer}
    </div>
    ${via}
    ${s.ok ? `<p class="meter"><span>${(s.w ?? 0).toFixed(1)} W</span><span>${s.v ?? '–'} V</span><span>${(s.kwh ?? 0).toFixed(3)} kWh total</span></p>` : ''}
    ${pendTxt}
    <div class="actions">
      <button class="${isOn ? 'sel-on' : ''}" aria-pressed="${isOn}" data-act="on" data-mac="${mac}">On</button>
      <button class="${isOff ? 'sel-off' : ''}" aria-pressed="${isOff}" data-act="off" data-mac="${mac}">Off</button>
      <button data-act="timer" data-mac="${mac}">Timer…</button>
    </div>
    <h3>Schedules</h3>
    ${rules ? `<ul class="list">${rules}</ul>` : '<p class="muted small">No repeating schedules.</p>'}
    ${ones ? `<h3>One-time</h3><ul class="list">${ones}</ul>` : ''}
    <div class="actions">
      <button data-act="addRule" data-mac="${mac}">+ Repeating</button>
      <button data-act="addOnce" data-mac="${mac}">+ One-time</button>
    </div>
  </section>`;
}

function renderDevice() {
  const off = $('offline');
  const silent = status && nowS() - status.t > OFFLINE_AFTER;
  off.hidden = !silent;
  if (silent) off.textContent = `The controller hasn't reported since ${new Date(status.t * 1000).toLocaleString()}, so it's probably offline (power, cell signal, or its daily data limit). Commands are kept for 15 minutes in case it comes back.`;
  const d = $('devInfo');
  if (!status) { d.innerHTML = '<dt>Status</dt><dd>No report yet</dd>'; return; }
  const n = status.net || {};
  const rows = [
    ['Last report', `${fmtAgo(status.t)} (${new Date(status.t * 1000).toLocaleString()})`],
    ['Cell signal', `${signal(n.csq)}${n.op ? ' · network ' + esc(n.op) : ''}`],
    ['SIM data (est.)', `today ${kb(n.d || 0)} · this month ${kb(n.m || 0)} · all time ${kb(n.tot || 0)}${n.cap ? ' · <b>cap reached, paused</b>' : ''}`],
    ['Clock', status.ts || 'none'],
    ['Settings', pendingCfg ? 'saving… (waiting for controller)' : config ? 'up to date' : 'defaults'],
    ['Firmware', `${esc(status.fw)} · up ${Math.floor(status.up / 3600)} h`],
  ];
  if (status.gps) rows.push(['Location', `<a href="https://maps.google.com/?q=${status.gps[0]},${status.gps[1]}" target="_blank" rel="noopener">${status.gps[0]}, ${status.gps[1]}</a>`]);
  d.innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
  $('tz').value = (config || baseConfig()).tz;
}

// ---- dialogs ------------------------------------------------------------------
// Resolves with the clicked button's value ('save', 'delete', 'cancel'). Uses
// the form's submit event rather than the dialog's close event, which some
// browsers deliver late or not at all for background tabs.
function ask(dlg) {
  return new Promise((res) => {
    const form = dlg.querySelector('form');
    const onSubmit = (e) => {
      e.preventDefault();
      done(e.submitter?.value || 'save');
    };
    const onCancel = () => done('cancel');
    function done(v) {
      form.removeEventListener('submit', onSubmit);
      dlg.removeEventListener('cancel', onCancel);
      if (dlg.open) dlg.close(v);
      res(v);
    }
    form.addEventListener('submit', onSubmit);
    dlg.addEventListener('cancel', onCancel);
    dlg.showModal();
  });
}

async function editRule(mac, i) {
  const cfg = baseConfig();
  const pc = plugCfg(cfg, mac);
  const r = i == null ? { d: 127, s: 360, m: 120, e: 1, nm: '' } : pc.r[i];
  $('ruleTitle').textContent = i == null ? 'New schedule' : 'Edit schedule';
  $('ruleName').value = r.nm || '';
  $('ruleDays').innerHTML = DAYS.map((d, k) =>
    `<label class="chip"><input type="checkbox" value="${k}" ${r.d & (1 << k) ? 'checked' : ''}><span>${d}</span></label>`).join('');
  $('ruleStart').value = fmtHM(r.s);
  durOptions($('ruleDur'), DURS, DURS.includes(r.m) ? r.m : 120);
  $('ruleEn').checked = r.e !== 0;
  $('ruleDelete').hidden = i == null;
  const res = await ask($('ruleDlg'));
  if (res === 'delete') { pc.r.splice(i, 1); return saveConfig(cfg); }
  if (res !== 'save') return;
  const days = [...$('ruleDays').querySelectorAll('input:checked')].reduce((a, el) => a | (1 << +el.value), 0);
  if (!days) return alert('Pick at least one day.');
  const [hh, mm] = $('ruleStart').value.split(':').map(Number);
  const nr = { nm: $('ruleName').value.trim(), d: days, s: hh * 60 + mm, m: +$('ruleDur').value, e: $('ruleEn').checked ? 1 : 0 };
  if (i == null) pc.r.push(nr); else pc.r[i] = nr;
  if (pc.r.length > 8) return alert('Up to 8 schedules per plug.');
  await saveConfig(cfg);
}

async function addOnce(mac) {
  const start = new Date(Date.now() + 3600e3);
  start.setMinutes(0, 0, 0);
  const local = new Date(start - start.getTimezoneOffset() * 60e3).toISOString().slice(0, 16);
  $('onceAt').value = local;
  durOptions($('onceDur'), DURS, 120);
  if (await ask($('onceDlg')) !== 'save') return;
  const at = Math.floor(new Date($('onceAt').value).getTime() / 1000);
  if (!(at > nowS() - 60)) return alert('Pick a time in the future.');
  const cfg = baseConfig();
  const pc = plugCfg(cfg, mac);
  pc.o = [...(pc.o || []), { at, m: +$('onceDur').value }].sort((a, b) => a.at - b.at).slice(-8);
  await saveConfig(cfg);
}

async function plugSettings(mac) {
  const cfg = baseConfig();
  const pc = plugCfg(cfg, mac);
  $('plugName').value = pc.n || '';
  durOptions($('plugMax'), [0, 30, 60, 120, 180, 240, 360, 480, 720], pc.max || 0);
  $('plugMax').options[0].textContent = 'No limit';
  if (await ask($('plugDlg')) !== 'save') return;
  pc.n = $('plugName').value.trim() || pc.n;
  pc.max = +$('plugMax').value;
  await saveConfig(cfg);
}

async function timer(mac) {
  durOptions($('timerDur'), DURS, 60);
  if (await ask($('timerDlg')) !== 'save') return;
  await sendCmd('tmr', mac, { m: +$('timerDur').value });
}

document.addEventListener('click', async (e) => {
  const b = e.target.closest('[data-act]');
  if (!b) return;
  const { act, mac, i, at } = b.dataset;
  if (act === 'on' || act === 'off') return sendCmd(act, mac);
  if (act === 'timer') return timer(mac);
  if (act === 'addRule') return editRule(mac, null);
  if (act === 'editRule') return editRule(mac, +i);
  if (act === 'addOnce') return addOnce(mac);
  if (act === 'plugSettings') return plugSettings(mac);
  if (act === 'delOnce') {
    const cfg = baseConfig();
    const pc = plugCfg(cfg, mac);
    pc.o = (pc.o || []).filter((o) => o.at !== +at);
    return saveConfig(cfg);
  }
});

// ---- session --------------------------------------------------------------------
async function start(s) {
  session = s;
  $('login').hidden = true;
  $('app').hidden = false;
  $('who').hidden = false;
  $('whoName').textContent = s.user.toUpperCase();
  render();
  connect();
}

$('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  $('loginBtn').disabled = true;
  $('loginBtn').textContent = 'Signing in…';
  try {
    const s = await deriveSession($('user').value, $('pass').value);
    if ($('remember').checked) localStorage.setItem('plugctl', JSON.stringify({ u: s.user, k: b64(s.raw) }));
    await start(s);
  } catch (err) {
    $('loginErr').hidden = false;
    $('loginErr').textContent = 'Sign-in failed: ' + err.message;
  } finally {
    $('loginBtn').disabled = false;
    $('loginBtn').textContent = 'Sign in';
  }
});

$('logout').addEventListener('click', () => {
  try { localStorage.removeItem('plugctl'); } catch {}
  location.reload();
});
$('refresh').addEventListener('click', refresh);
$('tz').innerHTML = TIMEZONES.map(([v, l]) => `<option value="${v}">${l}</option>`).join('');
$('saveTz').addEventListener('click', async () => {
  const cfg = baseConfig();
  cfg.tz = $('tz').value;
  await saveConfig(cfg);
});
setInterval(() => session && render(), 30000);

(async () => {
  try {
    const saved = JSON.parse(localStorage.getItem('plugctl') || 'null');
    if (saved) await start(await sessionFromRaw(saved.u, unb64(saved.k)));
  } catch {}
})();
