// gpu.js — the GPU demo page.
//
// Draws only what the binaries and the driver report, and says so where it
// doesn't. Three things are deliberately visual rather than written down:
// where the buffer physically lives, where a challenge's time goes, and which
// of the two detectors is firing. The caveats are badges, not paragraphs.
'use strict';

const $ = (id) => document.getElementById(id);
const el = {};
['led','statusText','deviceText','themeBtn','runBtn','stopBtn','mode','modeHint',
 'offload','offloadLabel','offloadKnob','hog','hogLabel','hogKnob','frac','fracLabel','fracKnob',
 'mb','k','lanes','n','warmup','gap','verify','geomHint','gen','chain','nonce','fold',
 'residency','resLegend','resBadges','timesplit','timeLegend','detect','latency','latLegend',
 'cSm','cTemp','cGbps','cErr','cardBadges','log','synthetic',
 'baseText','baseBadges','clearBaseBtn','hist','clearHistBtn'].forEach(id => el[id] = $(id));

const MODE_HINTS = {
  honest:   'cudaMalloc, then cudaPointerGetAttributes must report device memory. If it reports anything else the prover exits at startup, before serving any challenge.',
  partial:  'Every byte held, every answer correct. A tail of the buffer sits in pinned host memory and the kernel reads it over PCIe, so its cost lands in kernel time, not in readback.',
  uvm:      'cudaMallocManaged. Pages migrate between host and card on fault and the driver will not report how many are where. Indistinguishable from honest while the buffer fits in free HBM.',
  zerocopy: 'cudaHostAlloc, mapped. No bytes on the card; every probe crosses PCIe.',
  fake:     'Holds a fraction and derives the rest from a constant. Fast and wrong — caught by the answer, not the clock.',
};

const HOG_STEPS = 16;
const STORE_KEY = 'memchal.gpu.v1';
const state = {
  phase: 'idle', cfg: null, challenges: [], gpu: [], smi: null, end: null,
  backing: null, device: null, hbmTotal: null, hbmUsed: null, residentFrac: null,
  baseline: null, history: [], synthetic: false, dirty: {}, pending: [],
};

// The geometry a measurement is only comparable within. A baseline taken at a
// different buffer size, probe count, lane count, or fold is a different
// experiment, so every ratio drawn against it would be meaningless.
const GEOM_KEYS = ['mb', 'k', 'lanes', 'fold', 'chain', 'gen', 'nonce'];
const geomOf = (cfg) => cfg ? GEOM_KEYS.map(k => `${k}=${cfg[k]}`).join(' ') : '';
const sameGeom = (a, b) => !!a && !!b && geomOf(a) === geomOf(b);

function save() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify({
      baseline: state.baseline, history: state.history.slice(-40),
    }));
  } catch (_) { /* private window, quota, blocked storage: the page still works */ }
}
function load() {
  try {
    const raw = localStorage.getItem(STORE_KEY);
    if (!raw) return;
    const d = JSON.parse(raw);
    if (d && d.baseline) state.baseline = d.baseline;
    if (d && Array.isArray(d.history)) state.history = d.history;
  } catch (_) { /* same */ }
}
const markDirty = () => { state.dirty = { res: 1, time: 1, detect: 1, lat: 1 }; };

// ── palette ─────────────────────────────────────────────────────────────────
const css = (n, f) => (getComputedStyle(document.documentElement).getPropertyValue(n).trim() || f);
const P = () => ({
  card:   css('--fast', '#3987e5'),      // on the card
  host:   css('--slow', '#d95926'),      // in host memory
  absent: css('--critical', '#e2445c'),  // not held at all
  good:   css('--good', '#39b56a'),
  axis:   css('--axis', '#5b6b73'),
  hair:   css('--hair', '#1e2a30'),
  ink:    css('--ink', '#e6edf0'),
  muted:  css('--muted', '#7d8f98'),
  mono:   css('--face-num', 'ui-monospace, monospace'),
});

const fmtUs = (u) => u == null ? '—' : u >= 1000 ? `${(u / 1000).toFixed(2)} ms` : `${u.toFixed(0)} µs`;
const fmtGiB = (b) => b == null ? '—' : `${(b / 1073741824).toFixed(1)} GiB`;
const pct = (a, q) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(q * s.length))]; };

function log(line, src) {
  const d = document.createElement('div');
  d.textContent = src ? `[${src}] ${line}` : line;
  el.log.appendChild(d);
  while (el.log.childElementCount > 300) el.log.removeChild(el.log.firstChild);
  el.log.scrollTop = el.log.scrollHeight;
}

// ── canvas sizing ───────────────────────────────────────────────────────────
// The CSS box is authoritative. Assigning canvas.height rewrites the height
// attribute, so reading that attribute back next frame returns what we wrote,
// scaled again — the canvas then doubles every redraw until the tab dies.
// gpu.css gives every canvas here an explicit height for exactly this reason.
const MAX_PX = 2000;
function fit(c) {
  const dpr = window.devicePixelRatio || 1;
  if (!c.dataset.baseH) {
    // First call only, and this is the whole fix. Every way of asking a canvas
    // how tall it is — clientHeight, the height attribute, computed style —
    // reports the bitmap once CSS does not pin it, and the bitmap is what we
    // are about to set. So read it exactly once, remember it, and never ask
    // again. gpu.css also pins each canvas, which makes the first read right;
    // this makes every later read unnecessary.
    const initial = c.clientHeight || Number(c.getAttribute('height')) || 160;
    c.dataset.baseH = String(Math.max(40, Math.min(initial, MAX_PX)));
    c.style.height = c.dataset.baseH + 'px';
  }
  const h = Number(c.dataset.baseH);
  const w = c.clientWidth || 600;   // width is CSS-driven (app.css: canvas{width:100%})
  const bw = Math.round(w * dpr), bh = Math.round(h * dpr);
  if (c.width !== bw || c.height !== bh) { c.width = bw; c.height = bh; }
  const ctx = c.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}
function legend(node, items) {
  node.innerHTML = '';
  for (const [color, text] of items) {
    const s = document.createElement('span');
    s.innerHTML = `<i style="background:${color}"></i>${text}`;
    node.appendChild(s);
  }
}
function hatch(ctx, x, y, w, h, color) {
  ctx.save(); ctx.beginPath(); ctx.rect(x, y, w, h); ctx.clip();
  ctx.strokeStyle = color; ctx.globalAlpha = 0.55; ctx.lineWidth = 1;
  for (let i = -h; i < w + h; i += 6) {
    ctx.beginPath(); ctx.moveTo(x + i, y + h); ctx.lineTo(x + i + h, y); ctx.stroke();
  }
  ctx.restore();
}
function label(ctx, text, x, y, color, size = 10, weight = 600) {
  ctx.fillStyle = color;
  ctx.font = `${weight} ${size}px ${P().mono}`;
  ctx.fillText(text, x, y);
}

// ── 1. where the buffer lives ───────────────────────────────────────────────
// The thing every cheat manipulates, drawn as the thing it is: a block of bytes
// split across places with very different speeds.
function drawResidency() {
  const { ctx, w, h } = fit(el.residency);
  const p = P();
  const mode = state.cfg ? state.cfg.mode : 'honest';
  let onCard = state.residentFrac;
  if (onCard == null) onCard = mode === 'zerocopy' ? 0 : 1;
  const absent = (mode === 'fake' && state.cfg) ? 1 - (state.cfg.frac ?? 0.5) : 0;
  const card = Math.max(0, onCard - absent);
  const host = Math.max(0, 1 - card - absent);

  legend(el.resLegend, [[p.card, 'on the card'], [p.host, 'host memory'],
                        ...(absent > 0 ? [[p.absent, 'not held']] : [])]);

  const pad = 14, barH = 34, y = 16;
  const bw = w - pad * 2;
  let x = pad;
  for (const [frac, color] of [[card, p.card], [host, p.host], [absent, p.absent]]) {
    if (frac <= 0) continue;
    ctx.fillStyle = color;
    ctx.fillRect(x, y, bw * frac, barH);
    if (frac > 0.08) {
      label(ctx, `${Math.round(frac * 100)}%`, x + 6, y + barH / 2 + 4, '#0b0f10', 11, 700);
    }
    x += bw * frac;
  }
  ctx.strokeStyle = p.hair; ctx.strokeRect(pad, y, bw, barH);

  const mb = state.cfg ? state.cfg.mb : 0;
  label(ctx, `${mb} MiB challenge buffer`, pad, y - 4, p.muted, 10, 500);

  // HBM capacity underneath, so "does it fit" is visible rather than asserted.
  if (state.hbmTotal) {
    const cy = y + barH + 20;
    const usedFrac = Math.min(1, (state.hbmUsed || 0) / state.hbmTotal);
    ctx.fillStyle = p.hair; ctx.fillRect(pad, cy, bw, 10);
    ctx.fillStyle = p.card; ctx.fillRect(pad, cy, bw * usedFrac, 10);
    label(ctx, `card: ${fmtGiB(state.hbmUsed)} of ${fmtGiB(state.hbmTotal)} allocated`,
          pad, cy + 22, p.muted, 10, 500);
  }

  const badges = [];
  if (mode === 'uvm') badges.push(['warn', 'driver will not report how much migrated']);
  if (state.backing) badges.push(['', state.backing]);
  el.resBadges.innerHTML = badges
    .map(([c, t]) => `<span class="badge ${c}">${t}</span>`).join('');
}

// ── 2. where the time goes ──────────────────────────────────────────────────
// The honest baseline is drawn above the current run at the same scale, so a
// slowdown shows as a bar getting longer rather than as a ratio to read.
function drawTime() {
  const { ctx, w, h } = fit(el.timesplit);
  const p = P();
  // 'result readback' rather than 'pcie copy': copy_ms is only the answer
  // coming back (8 bytes for the tree fold, lanes x 8 for the sequential one).
  // A partial cheat's PCIe reads happen inside the kernel, so they are counted
  // in kernel time — which is where the stall actually is.
  legend(el.timeLegend, [[p.card, 'kernel (incl. any host-memory probes)'],
                         [p.good, 'result readback'],
                         [p.host, 'fold'], [p.muted, 'residual']]);

  const rows = [];
  const base = usableBaseline();
  if (base) rows.push({ name: 'honest', ...base });
  const cur = currentSplit();
  if (cur) rows.push({ name: state.cfg ? state.cfg.mode : 'run', ...cur });
  if (!rows.length) {
    label(ctx, 'run something', 14, h / 2, p.muted, 12, 500);
    return;
  }

  const max = Math.max(...rows.map(r => r.total));
  const pad = { l: 66, r: 14, t: 18 };
  const bw = w - pad.l - pad.r;
  const barH = 26, gap = 16;

  rows.forEach((r, i) => {
    const y = pad.t + i * (barH + gap);
    label(ctx, r.name, 10, y + barH / 2 + 4, r.name === 'honest' ? p.muted : p.ink, 11, 700);
    let x = pad.l;
    const segs = [[r.kernel, p.card], [r.copy, p.good], [r.fold, p.host]];
    for (const [v, color] of segs) {
      const px = bw * (v / max);
      ctx.fillStyle = color; ctx.globalAlpha = r.name === 'honest' ? 0.35 : 1;
      ctx.fillRect(x, y, px, barH); ctx.globalAlpha = 1;
      x += px;
    }
    const rest = bw * (Math.max(0, r.total - r.kernel - r.copy - r.fold) / max);
    hatch(ctx, x, y, rest, barH, p.muted);
    ctx.strokeStyle = p.hair; ctx.strokeRect(pad.l, y, bw * (r.total / max), barH);
    label(ctx, fmtUs(r.total * 1000), pad.l + bw * (r.total / max) + 6, y + barH / 2 + 4,
          p.muted, 10, 600);
  });
}

// The stored baseline, but only when it describes the same experiment. A
// baseline from a different buffer size or lane count is not a baseline.
function usableBaseline() {
  if (!state.baseline) return null;
  if (!state.cfg) return state.baseline;
  return sameGeom(state.baseline.cfg, state.cfg) ? state.baseline : null;
}

// Median of the traced per-challenge costs, in ms, plus the round trip.
function currentSplit() {
  if (!state.gpu.length) return null;
  const k = pct(state.gpu.map(g => g.kernel_ms), 0.5);
  const c = pct(state.gpu.map(g => g.copy_ms), 0.5);
  const f = pct(state.gpu.map(g => g.fold_ms), 0.5);
  const meas = state.challenges.filter(x => x.ph !== 'warmup').map(x => x.us);
  const rt = pct(meas, 0.5);
  if (rt == null) return null;
  return { kernel: k, copy: c, fold: f, total: rt / 1000 };
}

// ── 3. detector state ───────────────────────────────────────────────────────
// x: response p50 relative to the honest baseline. y: share of CHECKED answers
// that were wrong. Two independent detectors, so two axes; a prover leaving the
// origin in either direction is caught, and the axis says by which one.
function drawDetect() {
  const { ctx, w, h } = fit(el.detect);
  const p = P();
  const pad = { l: 44, r: 16, t: 16, b: 30 };
  const pw = w - pad.l - pad.r, ph = h - pad.t - pad.b;

  // x: timing ratio, log from 1x to 100x. y: share of answers wrong.
  const X = (r) => pad.l + pw * Math.min(1, Math.log10(Math.max(1, r)) / 2);
  const Y = (e) => pad.t + ph * (1 - Math.min(1, e));

  ctx.fillStyle = p.hair; ctx.globalAlpha = 0.5;
  ctx.fillRect(X(1.5), pad.t, pad.l + pw - X(1.5), ph);          // caught: slow
  ctx.fillRect(pad.l, pad.t, pw, Y(0.02) - pad.t);               // caught: wrong
  ctx.globalAlpha = 1;

  ctx.strokeStyle = p.hair;
  ctx.strokeRect(pad.l, pad.t, pw, ph);
  for (const r of [1, 2, 5, 10, 100]) {
    ctx.beginPath(); ctx.moveTo(X(r), pad.t); ctx.lineTo(X(r), pad.t + ph);
    ctx.stroke();
    label(ctx, `${r}×`, X(r) - 6, pad.t + ph + 14, p.muted, 9, 500);
  }
  label(ctx, '100', 8, pad.t + 8, p.muted, 9, 500);
  label(ctx, '0', 8, pad.t + ph, p.muted, 9, 500);
  label(ctx, 'wrong answers (% of checked)', 8, pad.t - 5, p.muted, 9, 600);
  label(ctx, 'response p50 / honest p50 (x, log)', pad.l, pad.t + ph + 26, p.muted, 9, 600);

  label(ctx, 'correctness detector', pad.l + 6, pad.t + 12, p.absent, 9, 700);
  label(ctx, 'timing detector', X(1.5) + 6, pad.t + ph - 8, p.host, 9, 700);

  const meas = state.challenges.filter(x => x.ph !== 'warmup');
  if (!meas.length) { label(ctx, 'no measured challenges', pad.l + 8, pad.t + ph / 2, p.muted, 11, 500); return; }

  // Only challenges the verifier actually recomputed can be right or wrong.
  // Dividing by every challenge instead scaled the rate down by exactly the
  // sampling factor, so a prover wrong on all of them plotted at 1/verify.
  const checked = meas.filter(x => x.checked !== false);
  const errRate = checked.length ? checked.filter(x => !x.ok).length / checked.length : 0;
  const p50 = pct(meas.map(x => x.us), 0.5);
  const base = usableBaseline();
  const ratio = base ? p50 / (base.total * 1000) : 1;
  if (!base) {
    label(ctx, 'no comparable baseline — x pinned at 1x',
          pad.l + 6, pad.t + ph - 22, p.muted, 9, 600);
  }
  if (checked.length && checked.length < meas.length) {
    label(ctx, `${checked.length} of ${meas.length} answers checked`,
          pad.l + 6, pad.t + 26, p.muted, 9, 500);
  }

  ctx.fillStyle = p.good;
  ctx.beginPath(); ctx.arc(X(1), Y(0), 4, 0, 7); ctx.fill();
  label(ctx, 'honest', X(1) + 8, Y(0) - 6, p.good, 9, 600);

  const caught = errRate > 0.02 || ratio > 1.5;
  ctx.fillStyle = caught ? p.absent : p.ink;
  ctx.beginPath(); ctx.arc(X(ratio), Y(errRate), 6, 0, 7); ctx.fill();
  label(ctx, state.cfg ? state.cfg.mode : '', X(ratio) + 10, Y(errRate) + 4,
        caught ? p.absent : p.ink, 11, 700);
}

// ── 4. round trip per challenge ─────────────────────────────────────────────
function downsample(pts, target) {
  if (pts.length <= target) return pts;
  const out = [], step = pts.length / target;
  for (let b = 0; b < target; b++) {
    const lo = Math.floor(b * step), hi = Math.min(pts.length, Math.floor((b + 1) * step));
    if (hi <= lo) continue;
    let worst = pts[lo], anyErr = false, anyWarm = false;
    for (let j = lo; j < hi; j++) {
      if (pts[j].us > worst.us) worst = pts[j];
      if (!pts[j].ok) anyErr = true;
      if (pts[j].ph === 'warmup') anyWarm = true;
    }
    out.push({ us: worst.us, ok: !anyErr, ph: anyWarm ? 'warmup' : 'measured' });
  }
  return out;
}

function drawLatency() {
  const { ctx, w, h } = fit(el.latency);
  const p = P();
  const raw = state.challenges;
  const pts = downsample(raw, Math.max(200, Math.floor(el.latency.clientWidth || 600)));
  legend(el.latLegend, [[p.card, 'measured'], [p.hair, 'warmup'], [p.absent, 'wrong']]);
  if (!pts.length) { label(ctx, 'no challenges yet', 14, h / 2, p.muted, 12, 500); return; }

  const pad = { l: 56, r: 12, t: 12, b: 18 };
  const hi = Math.max(...pts.map(x => x.us)) * 1.1;
  const X = (i) => pad.l + (w - pad.l - pad.r) * (pts.length === 1 ? 0.5 : i / (pts.length - 1));
  const Y = (v) => h - pad.b - (h - pad.t - pad.b) * (v / (hi || 1));

  ctx.strokeStyle = p.hair;
  for (let g = 0; g <= 3; g++) {
    const v = (hi * g) / 3;
    ctx.beginPath(); ctx.moveTo(pad.l, Y(v)); ctx.lineTo(w - pad.r, Y(v)); ctx.stroke();
    label(ctx, fmtUs(v), 6, Y(v) + 3, p.muted, 9, 500);
  }
  const baseLat = usableBaseline();
  if (baseLat) {
    const b = baseLat.total * 1000;
    if (b < hi) {
      ctx.setLineDash([4, 4]); ctx.strokeStyle = p.good;
      ctx.beginPath(); ctx.moveTo(pad.l, Y(b)); ctx.lineTo(w - pad.r, Y(b)); ctx.stroke();
      ctx.setLineDash([]);
      label(ctx, 'honest', pad.l + 4, Y(b) - 4, p.good, 9, 600);
    }
  }
  ctx.beginPath();
  pts.forEach((c, i) => i ? ctx.lineTo(X(i), Y(c.us)) : ctx.moveTo(X(i), Y(c.us)));
  ctx.strokeStyle = p.card; ctx.lineWidth = 1.3; ctx.stroke();
  pts.forEach((c, i) => {
    if (c.ph === 'warmup') ctx.fillStyle = p.hair;
    else if (!c.ok) ctx.fillStyle = p.absent;
    else return;
    ctx.beginPath(); ctx.arc(X(i), Y(c.us), 2, 0, 7); ctx.fill();
  });
}

// ── readouts ────────────────────────────────────────────────────────────────
function updateReadouts() {
  const s = state.smi;
  el.cSm.textContent = s && s.sm_mhz != null ? s.sm_mhz.toFixed(0) : '—';
  el.cTemp.textContent = s && s.temp_c != null ? s.temp_c.toFixed(0) : '—';
  const g = pct(state.gpu.map(x => x.probe_gbps), 0.5);
  el.cGbps.textContent = g == null ? '—' : g.toFixed(1);
  const meas = state.challenges.filter(x => x.ph !== 'warmup');
  const errs = meas.filter(x => !x.ok).length;
  el.cErr.textContent = meas.length ? `${errs}/${meas.length}` : '—';
  el.cErr.style.color = errs ? P().absent : '';

  const b = [];
  if (s && s.throttle && !/^(Not Active|0x0*1?)$/i.test(s.throttle.trim())) {
    b.push(`<span class="badge warn">throttling: ${s.throttle}</span>`);
  }
  b.push('<span class="badge">clocks unlocked — ratios only</span>');
  b.push('<span class="badge">clock sampled between challenges, not during</span>');
  if (state.end && state.end.checked != null && state.end.checked < state.end.samples) {
    b.push(`<span class="badge warn">${state.end.checked} of ${state.end.samples} answers checked</span>`);
  }
  el.cardBadges.innerHTML = b.join('');
}

// ── baseline and history panels ─────────────────────────────────────────────
function renderBaseline() {
  const b = state.baseline;
  if (!b) {
    el.baseText.textContent = 'no honest run yet — ratios unavailable';
    el.baseBadges.innerHTML = '';
    return;
  }
  const age = Math.round((Date.now() - (b.at || 0)) / 1000);
  const ageTxt = age < 90 ? `${age}s ago` : `${Math.round(age / 60)} min ago`;
  el.baseText.textContent = `${fmtUs(b.total * 1000)} · ${ageTxt} · ${geomOf(b.cfg)}`;
  const badges = [];
  if (state.cfg && !sameGeom(b.cfg, state.cfg)) {
    badges.push('<span class="badge warn">geometry differs from current run</span>');
  }
  if (age > 900) {
    badges.push('<span class="badge warn">clocks unlocked — re-measure</span>');
  }
  el.baseBadges.innerHTML = badges.join('');
}

function renderHistory() {
  const rows = state.history.slice().reverse();
  if (!rows.length) {
    el.hist.innerHTML = '<tr><td class="muted">no runs yet</td></tr>';
    return;
  }
  const head = '<tr><th>mode</th><th>p50</th><th>vs base</th><th>wrong</th>'
             + '<th>kernel</th><th>readback</th><th>fold</th><th>geometry</th></tr>';
  const body = rows.map(r => {
    const b = state.baseline;
    const cmp = b && sameGeom(b.cfg, r.cfg)
      ? `${(r.p50 / (b.total * 1000)).toFixed(2)}x` : '—';
    const den = r.checked != null ? r.checked : r.samples;
    const wrong = r.errors ? `<b class="bad">${r.errors}/${den}</b>` : `0/${den}`;
    return `<tr><td>${r.mode}</td><td>${fmtUs(r.p50)}</td><td>${cmp}</td>`
         + `<td>${wrong}</td><td>${fmtUs(r.kernel * 1000)}</td>`
         + `<td>${fmtUs(r.copy * 1000)}</td><td>${fmtUs(r.fold * 1000)}</td>`
         + `<td class="muted">${geomOf(r.cfg)}</td></tr>`;
  }).join('');
  el.hist.innerHTML = head + body;
}

// ── controls ────────────────────────────────────────────────────────────────
// The reserve slider used to take a fraction of the CARD (up to 90% of total),
// which cannot reach the cliff for a small buffer: 90% of an 80 GiB card still
// leaves 8 GiB free, so a 1 GiB managed buffer always fits and uvm always looks
// honest. The experiment is about free HBM relative to the BUFFER, so the
// slider now sets that ratio directly, geometrically from 8x down to 0.25x.
// The crossover (1x) then sits near 60% of the travel at any buffer size.
const FREE_HI = 8, FREE_LO = 0.25;
function freeRatio() {
  const pos = Number(el.hog.value) || 0;
  if (!pos) return null;                       // 0 = reserve nothing
  return FREE_HI * Math.pow(FREE_LO / FREE_HI, pos / HOG_STEPS);
}
function hogMib() {
  const r = freeRatio();
  if (r == null || !state.hbmTotal) return 0;
  const totalMib = state.hbmTotal / 1048576;
  const mb = Number(el.mb.value) || 0;
  return Math.max(0, Math.floor(totalMib - r * mb));
}

function syncControls() {
  const mode = el.mode.value;
  el.modeHint.textContent = MODE_HINTS[mode] || '';
  el.offloadKnob.style.display = mode === 'partial' ? '' : 'none';
  el.hogKnob.style.display = (mode === 'uvm' || mode === 'zerocopy') ? '' : 'none';
  el.fracKnob.style.display = mode === 'fake' ? '' : 'none';
  el.offloadLabel.textContent = `${el.offload.value}%`;
  el.fracLabel.textContent = `${el.frac.value}%`;
  const r = freeRatio();
  const mib = hogMib();
  if (r == null) {
    el.hogLabel.textContent = 'all';
  } else if (!state.hbmTotal) {
    el.hogLabel.textContent = `${r.toFixed(2)}x buffer`;
  } else {
    const freeMib = state.hbmTotal / 1048576 - mib;
    el.hogLabel.textContent =
      `${(freeMib / 1024).toFixed(1)} GiB · ${r.toFixed(2)}x buffer`;
  }

  const k = Number(el.k.value) || 1, lanes = Number(el.lanes.value) || 1;
  const depth = Math.floor(k / lanes);
  const bits = [`depth ${depth}`];
  if (el.chain.value === 'dependent' && depth < 16) {
    bits.push(`— too shallow, needs k ≥ ${(16 * lanes).toLocaleString()}`);
  }
  const mb = Number(el.mb.value) || 0;
  if (mb >= 4096) bits.push(`— ~${Math.round(mb / 250)}s to upload before the first challenge`);
  // Wall clock is dominated by two host-side terms, not by the card: the pace
  // you asked for, and the verifier recomputing answers at ~100 ns per probe.
  // Break them out, because otherwise a 40 s run reads as a slow GPU when the
  // GPU's share of it is a tenth of a second.
  const ve = Number(el.verify.value) || 1;
  const N = (Number(el.n.value) || 0) + (Number(el.warmup.value) || 0);
  const paceS = (Number(el.gap.value) || 0) / 1e6 * N;
  const checkS = (k * 100e-9) / ve * N;
  const gpuS = 0.0003 * N;                 // ~300 us per response, order of magnitude
  const total = paceS + checkS + gpuS;
  const fmtS = (v) => v < 1 ? '<1s' : `${Math.round(v)}s`;
  bits.push(`— ${fmtS(total)} total: ${fmtS(paceS)} pacing, ${fmtS(checkS)} verify, ${fmtS(gpuS)} gpu`);
  if (ve > 1) bits.push(`(checking 1 in ${ve})`);
  el.geomHint.textContent = bits.join(' ');
  el.geomHint.style.color = (el.chain.value === 'dependent' && depth < 16) ? P().absent : '';
  markDirty();
}

// ── events ──────────────────────────────────────────────────────────────────
function handle(e) {
  if (e.synthetic) { state.synthetic = true; el.synthetic.classList.add('on'); }
  switch (e.t) {
    case 'status':
      state.phase = e.phase; state.cfg = e.cfg || state.cfg;
      el.statusText.textContent = e.error ? `error: ${e.error}`
        : e.phase === 'uploading' ? `uploading ${(e.mb || (e.cfg && e.cfg.mb) || 0)} MiB…` : e.phase;
      el.led.style.background = ['running', 'uploading'].includes(e.phase) ? P().good
        : e.phase === 'error' ? P().absent : P().muted;
      el.runBtn.disabled = ['running', 'starting prover', 'uploading'].includes(e.phase);
      if (e.error) log(e.error, 'bridge');
      break;
    case 'gpuinfo':
      state.device = e.name || state.device;
      if (e.hbm_total) state.hbmTotal = e.hbm_total;
      el.deviceText.textContent = state.device || 'no device';
      break;
    case 'cheatinfo':
      state.backing = e.backing;
      if (e.hbm_total) { state.hbmTotal = e.hbm_total; state.hbmUsed = e.hbm_used; }
      log(`${e.mode}: driver says "${e.backing}"`, 'prover');
      break;
    case 'run':
      state.cfg = e.cfg || state.cfg;
      state.challenges = []; state.gpu = []; state.end = null;
      if (state.cfg && state.cfg.mode === 'honest') state.backing = 'device — cudaMalloc, verified';
      log(`${e.mb} MiB, k=${e.k}, lanes=${e.lanes}, upload ${Number(e.upload_s).toFixed(1)}s`, 'verifier');
      break;
    case 'c':
      state.challenges.push({ us: e.us, ph: e.ph, ok: e.ok, checked: e.checked });
      if (state.challenges.length > 5000) state.challenges.shift();
      break;
    case 'g':
      state.gpu.push(e);
      if (state.gpu.length > 5000) state.gpu.shift();
      if (e.hbm_total) { state.hbmTotal = e.hbm_total; state.hbmUsed = e.hbm_used; }
      if (e.resident_frac != null) state.residentFrac = e.resident_frac;
      break;
    case 'end': {
      state.end = e;
      const split = currentSplit();
      if (state.cfg && state.cfg.mode === 'honest' && split) {
        state.baseline = { ...split, cfg: { ...state.cfg }, at: Date.now() };
        log(`baseline set: ${fmtUs(e.p50)} at ${geomOf(state.cfg)}`, 'bridge');
      }
      if (state.cfg && split) {
        state.history.push({
          at: Date.now(), mode: state.cfg.mode, cfg: { ...state.cfg },
          p50: e.p50, errors: e.errors, samples: e.samples, checked: e.checked,
          kernel: split.kernel, copy: split.copy, fold: split.fold,
        });
        state.history = state.history.slice(-40);
      }
      save();
      renderBaseline();
      renderHistory();
      log(`${e.samples} samples, ${e.errors} errors, p50 ${fmtUs(e.p50)}`, 'verifier');
      break;
    }
    case 'smi': state.smi = e; break;
    case 'log': log(e.line, e.src); break;
    default: break;
  }
  markDirty();
}

function pump() {
  if (!state.pending.length) return;
  state.pending.splice(0, state.pending.length).forEach(handle);
  updateReadouts();
}
function tick() {
  if (state.dirty.res) drawResidency();
  if (state.dirty.time) drawTime();
  if (state.dirty.detect) drawDetect();
  if (state.dirty.lat) drawLatency();
  state.dirty = {};
  requestAnimationFrame(tick);
}
function connect() {
  const src = new EventSource('/events');
  src.onmessage = (m) => { try { state.pending.push(JSON.parse(m.data)); } catch (_) {} };
  src.onerror = () => { el.statusText.textContent = 'bridge disconnected'; };
}

// ── wiring ──────────────────────────────────────────────────────────────────
el.runBtn.addEventListener('click', async () => {
  el.log.innerHTML = '';
  const body = {
    mode: el.mode.value, gen: el.gen.value, chain: el.chain.value,
    nonce: el.nonce.value, fold: el.fold.value,
    mb: +el.mb.value, k: +el.k.value, lanes: +el.lanes.value,
    n: +el.n.value, warmup: +el.warmup.value, gap: +el.gap.value,
    verify: +el.verify.value,
    hog: hogMib(), trace: 1,
    frac: (+el.frac.value) / 100, offload: (+el.offload.value) / 100,
  };
  try {
    const r = await fetch('/run', { method: 'POST', body: JSON.stringify(body) });
    if (!r.ok) log((await r.json()).error || 'refused', 'bridge');
  } catch (err) { log(String(err), 'bridge'); }
});
el.stopBtn.addEventListener('click', () => fetch('/stop', { method: 'POST' }).catch(() => {}));
el.themeBtn.addEventListener('click', () => {
  const d = document.documentElement.dataset;
  d.theme = d.theme === 'dark' ? 'light' : 'dark';
  markDirty();
});
el.clearBaseBtn.addEventListener('click', () => {
  state.baseline = null; save(); renderBaseline(); markDirty();
});
el.clearHistBtn.addEventListener('click', () => {
  state.history = []; save(); renderHistory();
});
['input', 'change'].forEach(ev =>
  ['mode', 'offload', 'hog', 'frac', 'k', 'lanes', 'mb', 'chain']
    .forEach(id => el[id].addEventListener(ev, () => { syncControls(); renderBaseline(); })));
window.addEventListener('resize', markDirty);

load();
setInterval(pump, 40);
setInterval(renderBaseline, 30000);   // the age readout, so a stale baseline says so
syncControls();
updateReadouts();
renderBaseline();
renderHistory();
connect();
requestAnimationFrame(tick);
