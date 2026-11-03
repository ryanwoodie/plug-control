// Plug history: the controller's change logs (firmware/src/histlog.h), filed
// hourly by tools/collect_history.py into the `history` branch as sealed
// "<seq> <base64>" lines, plus any batches still on the broker (not filed
// yet). Uses app.js globals: session, open(), esc(), $, nowS.
'use strict';

let HISTORY_BASE = 'https://raw.githubusercontent.com/ryanwoodie/plug-control/history/history/';
const DRAW_W = 3;      // matches HIST_DRAW_W in firmware
const RAPID_S = 30;    // heat cycles shorter than this are flagged
const GAP_S = 90;      // missing coverage longer than this shows as "no data"
const WINDOWS = [[86400, 'Whole day'], [6 * 3600, '6 h'], [3600, '1 h'], [900, '15 min']];

const hist = {
  batches: new Map(),   // seq -> decoded batch
  months: new Map(),    // 'YYYY-MM' -> Promise (loaded once per page view)
  mac: null, day: 0, win: 86400, from: 0,
};

const histBase = () => session.topics.c.slice(0, -1);   // 'plugctl/<tid>/'
const histTid = () => histBase().split('/')[1];

function decodeBatch(b) {
  const bytes = Uint8Array.from(atob(b.e || ''), (c) => c.charCodeAt(0));
  const ev = [];
  let i = 0, t = b.a;
  const varint = () => {
    let v = 0, s = 0, x;
    do { x = bytes[i++]; v += (x & 0x7f) * 2 ** s; s += 7; } while (x & 0x80);
    return v;
  };
  while (i < bytes.length) {
    const v = varint();
    t += Math.floor(v / 4);
    const kind = v & 3;
    ev.push({ t, kind, w: kind === 0 ? varint() : null });
  }
  return { mac: b.p, seq: b.q, a: b.a, b: b.b, start: { ok: !!b.s[0], on: !!b.s[1], w: b.s[2] }, ev };
}

function histAdd(msg) {
  if (msg && msg.q && msg.p) hist.batches.set(msg.q, decodeBatch(msg));
}

async function loadMonth(ym) {
  if (!hist.months.has(ym)) {
    hist.months.set(ym, (async () => {
      const r = await fetch(`${HISTORY_BASE}${histTid()}/${ym}.txt`, { cache: 'no-cache' });
      if (!r.ok) return;
      for (const line of (await r.text()).split('\n')) {
        const [q, b64] = line.split(' ');
        if (!b64 || hist.batches.has(+q)) continue;
        try { histAdd(await open(`${histBase()}h/${q}`, unb64(b64))); } catch {}
      }
    })().catch(() => {}));
  }
  return hist.months.get(ym);
}

// Spans of constant state [{t0, t1, ok, on, w}] for one plug, sorted. Time
// not covered by any batch is left out (it shows as "no data").
function spansFor(mac) {
  const bs = [...hist.batches.values()].filter((b) => b.mac === mac).sort((x, y) => x.a - y.a);
  const out = [];
  for (const b of bs) {
    let st = { ...b.start }, t = b.a;
    const push = (t1) => { if (t1 > t) out.push({ t0: t, t1, ok: st.ok, on: st.on, w: st.w }); t = Math.max(t, t1); };
    for (const e of b.ev) {
      push(e.t);
      if (e.kind === 0) st.w = e.w;
      else if (e.kind === 1) st.on = false, st.ok = true;
      else if (e.kind === 2) st.on = true, st.ok = true;
      else st.ok = false;
    }
    push(b.b);
  }
  return out;
}

function clip(spans, t0, t1) {
  return spans.filter((s) => s.t1 > t0 && s.t0 < t1).map((s) => ({ ...s, t0: Math.max(s.t0, t0), t1: Math.min(s.t1, t1) }));
}

// Heat cycles: runs of drawing (> DRAW_W) while the plug answered.
function cycles(spans) {
  const out = [];
  let cur = null;
  for (const s of spans) {
    const drawing = s.ok && s.w > DRAW_W;
    if (drawing && cur && s.t0 - cur.t1 < 1) { cur.t1 = s.t1; cur.wh += s.w * (s.t1 - s.t0) / 3600; continue; }
    if (cur) out.push(cur), cur = null;
    if (drawing) cur = { t0: s.t0, t1: s.t1, wh: s.w * (s.t1 - s.t0) / 3600 };
  }
  if (cur) out.push(cur);
  return out;
}

function stats(spans, t0, t1) {
  const metered = spans.some((s) => s.w >= 0);
  let onS = 0, drawS = 0, wh = 0, cover = 0;
  for (const s of spans) {
    const d = s.t1 - s.t0;
    if (!s.ok) continue;
    cover += d;
    if (s.on) onS += d;
    if (s.w > DRAW_W) drawS += d, wh += s.w * d / 3600;
  }
  const cyc = cycles(spans);
  // Min/max/average only over cycles that start and end inside the window.
  const whole = cyc.filter((c) => c.t0 > t0 && c.t1 < t1).map((c) => c.t1 - c.t0);
  return {
    metered, onS, drawS, wh, cover, cyc,
    shortest: whole.length ? Math.min(...whole) : null,
    longest: whole.length ? Math.max(...whole) : null,
    avg: whole.length ? whole.reduce((a, b) => a + b, 0) / whole.length : null,
    rapid: cyc.filter((c) => c.t1 - c.t0 < RAPID_S && c.t0 > t0 && c.t1 < t1).length,
  };
}

// ---- view ---------------------------------------------------------------------
const fmtSecs = (s) => (s == null ? '–' : s < 90 ? `${Math.round(s)} s` : s < 5400 ? `${Math.round(s / 60)} min` : `${(s / 3600).toFixed(1)} h`);
const fmtClock = (t, secs) => new Date(t * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', ...(secs ? { second: '2-digit' } : {}) });
const dayStart = (t) => { const d = new Date(t * 1000); d.setHours(0, 0, 0, 0); return d.getTime() / 1000; };
const monthOf = (t) => { const d = new Date(t * 1000); return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, '0')}`; };

async function openHistory(mac) {
  hist.mac = mac;
  hist.day = dayStart(nowS());
  hist.win = 86400;
  hist.from = hist.day;
  $('plugs').hidden = true;
  $('device').hidden = true;
  $('history').hidden = false;
  window.scrollTo(0, 0);
  await histLoadAndDraw();
}

function closeHistory() {
  $('history').hidden = true;
  $('plugs').hidden = false;
  $('device').hidden = false;
}

async function histLoadAndDraw() {
  const t1 = hist.day + 86400;
  histDraw(true);
  // A batch is filed in the month it arrived, which can be after its day.
  const months = new Set([monthOf(hist.day), monthOf(t1), monthOf(t1 + 86400 * 3)]);
  await Promise.all([...months].filter((m) => m <= monthOf(nowS())).map(loadMonth));
  histDraw(false);
}

function histDraw(loading) {
  const el = $('history');
  const name = config?.p?.[hist.mac]?.n || status?.p?.[hist.mac]?.n || hist.mac;
  const t0 = hist.from, t1 = Math.min(hist.from + hist.win, hist.day + 86400);
  const spans = clip(spansFor(hist.mac), t0, t1);
  const st = stats(spans, t0, t1);
  const dayLabel = new Date(hist.day * 1000).toLocaleDateString([], { weekday: 'short', month: 'short', day: 'numeric' });
  const zoomed = hist.win < 86400;
  const tile = (label, value, note = '') => `<div class="tile"><span class="tl">${label}</span><span class="tv">${value}</span>${note ? `<span class="tn">${note}</span>` : ''}</div>`;
  const tiles = st.metered ? [
    tile('Heat cycles', st.cyc.length, st.rapid ? `<span class="flag">⚠ ${st.rapid} under ${RAPID_S} s</span>` : ''),
    tile('Drawing', fmtSecs(st.drawS), st.onS ? `${Math.round(100 * st.drawS / st.onS)}% of plug-on time` : ''),
    tile('Cycle length', fmtSecs(st.avg), st.shortest != null ? `${fmtSecs(st.shortest)} – ${fmtSecs(st.longest)}` : ''),
    tile('Avg while drawing', st.drawS ? `${Math.round(st.wh * 3600 / st.drawS)} W` : '–'),
    tile('Energy', `${(st.wh / 1000).toFixed(3)} kWh`),
    tile('Plug on', fmtSecs(st.onS)),
  ] : [tile('Plug on', fmtSecs(st.onS)), tile('Times switched on', spans.filter((s, i) => s.on && s.ok && !(spans[i - 1]?.on)).length)];
  const empty = !spans.length;
  el.innerHTML = `
    <section class="card">
      <div class="row between">
        <button class="link" id="histBack">← Back</button>
        <span class="muted small">${loading ? 'Loading…' : 'Filed hourly, so the last hour may not show yet'}</span>
      </div>
      <h2>${esc(name)}</h2>
      <div class="row hist-nav">
        <button data-h="prevDay" aria-label="Previous day">◀</button>
        <b>${dayLabel}</b>
        <button data-h="nextDay" aria-label="Next day" ${hist.day + 86400 > nowS() ? 'disabled' : ''}>▶</button>
        <select id="histWin" aria-label="Time window">${WINDOWS.map(([s, l]) => `<option value="${s}" ${s === hist.win ? 'selected' : ''}>${l}</option>`).join('')}</select>
        ${zoomed ? `<span class="pan"><button data-h="back" aria-label="Earlier">‹</button><span class="small">${fmtClock(t0)} – ${fmtClock(t1)}</span><button data-h="fwd" aria-label="Later">›</button></span>` : ''}
      </div>
      <div class="tiles ${loading ? 'dim' : ''}">${tiles.join('')}</div>
      ${!st.metered && !empty ? '<p class="muted small">This plug has no power meter, so this shows when it was switched on.</p>' : ''}
      <div class="chart ${loading ? 'dim' : ''}" id="histChart">${empty && !loading ? '<p class="muted">No history for this time. Recording starts once the controller has the history update.</p>' : ''}</div>
      <p class="key small muted"><span class="ki"><i class="k-on"></i>Plug on</span><span class="ki"><i class="k-gap"></i>No data</span>${st.metered ? '<span>Drag across the chart or pick a window to zoom</span>' : ''}</p>
      ${st.metered && st.cyc.length ? `<details><summary>Heat cycles (${st.cyc.length})</summary>
        <table class="cyc"><thead><tr><th>Start</th><th>Length</th><th>Avg W</th><th>Off before</th></tr></thead><tbody>
        ${st.cyc.map((c, i) => `<tr class="${c.t1 - c.t0 < RAPID_S ? 'rapid' : ''}"><td>${fmtClock(c.t0, true)}</td><td>${fmtSecs(c.t1 - c.t0)}</td>
          <td>${Math.round(c.wh * 3600 / (c.t1 - c.t0))}</td><td>${i ? fmtSecs(c.t0 - st.cyc[i - 1].t1) : '–'}</td></tr>`).join('')}
        </tbody></table></details>` : ''}
    </section>`;
  if (!empty) drawChart($('histChart'), spans, t0, t1, st.metered);
}

function niceMax(v) {
  if (v <= 10) return 10;
  const p = 10 ** Math.floor(Math.log10(v)), m = v / p;
  return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 2.5 ? 2.5 : m <= 5 ? 5 : 10) * p;
}

function drawChart(el, spans, t0, t1, metered) {
  const W = el.clientWidth || 600, H = metered ? 220 : 56, L = metered ? 44 : 4, R = 4, T = 8, B = 22;
  const x = (t) => L + (t - t0) / (t1 - t0) * (W - L - R);
  const ymax = niceMax(Math.max(10, ...spans.map((s) => s.w || 0)) * 1.1);
  const y = (w) => T + (1 - w / ymax) * (H - T - B);
  // Time labels at least ~70 px apart, on round clock intervals.
  const tickS = [60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600].find((s) => (W - L - R) * s / (t1 - t0) >= 70) || 21600;
  let svg = `<svg width="${W}" height="${H}" role="img" aria-label="Power draw over time">
    <defs><pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
      <line x1="0" y1="0" x2="0" y2="6" class="hatch"/></pattern></defs>
    <rect x="${L}" y="${T}" width="${W - L - R}" height="${H - T - B}" fill="url(#hatch)"/>`;
  // Covered time is painted over the hatching; plug-on time gets a wash.
  // Merged first: thousands of thin touching rects would let the hatching
  // show through their anti-aliased edges.
  const bands = [];
  for (const s of spans) {
    if (!s.ok) continue;
    const b = bands.at(-1);
    if (b && b.on === s.on && s.t0 - b.t1 < 1) b.t1 = s.t1; else bands.push({ t0: s.t0, t1: s.t1, on: s.on });
  }
  for (const b of bands) svg += `<rect x="${x(b.t0)}" y="${T}" width="${Math.max(0.5, x(b.t1) - x(b.t0))}" height="${H - T - B}" class="${b.on ? (metered ? 'band-on' : 'band-on solid') : 'band-off'}"/>`;
  if (metered) {
    for (let w = 0; w <= ymax; w += ymax / 4) {
      svg += `<line x1="${L}" x2="${W - R}" y1="${y(w)}" y2="${y(w)}" class="grid"/><text x="${L - 6}" y="${y(w) + 4}" class="ax" text-anchor="end">${Math.round(w)}${w === ymax ? ' W' : ''}</text>`;
    }
    let line = '', area = '', prev = null;
    for (const s of spans) {
      if (!s.ok || s.w < 0) { prev = null; continue; }
      const a = x(s.t0), b = x(s.t1), v = y(s.w);
      if (prev && Math.abs(prev - s.t0) < 1) line += `V${v}H${b}`; else line += `M${a},${v}H${b}`;
      area += `M${a},${y(0)}V${v}H${b}V${y(0)}Z`;
      prev = s.t1;
    }
    svg += `<path d="${area}" class="area"/><path d="${line}" class="pline"/>`;
  }
  const firstTick = Math.ceil((t0 - dayStart(t0)) / tickS) * tickS + dayStart(t0);
  for (let t = firstTick; t <= t1; t += tickS) {
    const edge = x(t) < L + 28 || x(t) > W - R - 28;  // a label there would spill out or crowd its neighbour
    svg += `<line x1="${x(t)}" x2="${x(t)}" y1="${H - B}" y2="${H - B + 4}" class="grid"/>${edge ? '' : `<text x="${x(t)}" y="${H - 6}" class="ax" text-anchor="middle">${fmtClock(t)}</text>`}`;
  }
  svg += `<line id="hx" y1="${T}" y2="${H - B}" class="cross" visibility="hidden"/><rect id="hsel" y="${T}" height="${H - T - B}" class="sel" visibility="hidden"/></svg>
    <div class="tip" id="htip" hidden></div>`;
  el.innerHTML = svg;

  const svgEl = el.querySelector('svg'), tip = el.querySelector('#htip'), hx = el.querySelector('#hx'), sel = el.querySelector('#hsel');
  const tAt = (px) => t0 + (px - L) / (W - L - R) * (t1 - t0);
  let dragFrom = null;
  const px = (e) => e.clientX - svgEl.getBoundingClientRect().left;
  svgEl.addEventListener('pointermove', (e) => {
    const p = Math.min(W - R, Math.max(L, px(e))), t = tAt(p);
    const s = spans.find((z) => z.t0 <= t && t < z.t1);
    hx.setAttribute('x1', p); hx.setAttribute('x2', p); hx.setAttribute('visibility', 'visible');
    tip.hidden = false;
    tip.textContent = '';
    const v = document.createElement('b');
    v.textContent = !s || !s.ok ? 'No data' : metered ? `${s.w} W` : s.on ? 'On' : 'Off';
    const sub = document.createElement('span');
    sub.textContent = `${fmtClock(t, true)}${s && s.ok && metered ? ` · plug ${s.on ? 'on' : 'off'}` : ''}`;
    tip.append(v, sub);
    tip.style.left = `${Math.min(p + 10, W - 150)}px`;
    if (dragFrom != null) {
      sel.setAttribute('x', Math.min(dragFrom, p)); sel.setAttribute('width', Math.abs(p - dragFrom)); sel.setAttribute('visibility', 'visible');
    }
  });
  svgEl.addEventListener('pointerleave', () => { tip.hidden = true; hx.setAttribute('visibility', 'hidden'); });
  svgEl.addEventListener('pointerdown', (e) => { if (e.pointerType === 'mouse') dragFrom = Math.max(L, px(e)); });
  svgEl.addEventListener('pointerup', (e) => {
    if (dragFrom == null) return;
    const a = tAt(Math.min(dragFrom, px(e))), b = tAt(Math.max(dragFrom, px(e)));
    dragFrom = null;
    sel.setAttribute('visibility', 'hidden');
    if (b - a < 30) return;
    hist.from = Math.floor(a);
    hist.win = Math.ceil(b - a);
    histDraw(false);
  });
}

document.addEventListener('click', async (e) => {
  const b = e.target.closest('[data-h], #histBack');
  if (!b) return;
  if (b.id === 'histBack') return closeHistory();
  const h = b.dataset.h;
  if (h === 'prevDay' || h === 'nextDay') {
    const d = new Date(hist.day * 1000);
    d.setDate(d.getDate() + (h === 'prevDay' ? -1 : 1));
    hist.day = d.getTime() / 1000;
    hist.from = hist.day;
    hist.win = 86400;
    return histLoadAndDraw();
  }
  if (h === 'back' || h === 'fwd') {
    hist.from = Math.min(hist.day + 86400 - hist.win, Math.max(hist.day, hist.from + (h === 'back' ? -1 : 1) * hist.win * 0.75));
    histDraw(false);
  }
});

document.addEventListener('change', (e) => {
  if (e.target.id !== 'histWin') return;
  const mid = hist.from + hist.win / 2;
  hist.win = +e.target.value;
  hist.from = Math.max(hist.day, Math.min(hist.day + 86400 - hist.win, Math.round(mid - hist.win / 2)));
  histDraw(false);
});

window.addEventListener('resize', () => { if (!$('history').hidden) histDraw(false); });
