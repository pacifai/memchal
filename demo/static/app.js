/* app.js — memory-challenge live demo.
 *
 * Everything drawn here arrives from the real binaries over SSE (see
 * demo/bridge.py): per-challenge latency and correctness from the verifier, and
 * per-N-challenge counters + a mincore() residency bitmap from the prover. The
 * page map is not an illustration — the probed pages come from the verifier's own
 * index sequence and the colours come from the kernel's view of what is resident.
 *
 * Two honest-presentation rules the code keeps:
 *   1. x is challenge number, never wall-clock. Events are drained at a paced
 *      rate so a burst is legible, which is a drawing choice, not a timeline.
 *   2. Nothing is inferred from a colour alone — every series and page state is
 *      named in a legend, and the same facts appear numerically in the tiles.
 */
'use strict';

const MEM_STEPS = [32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024]; // MiB
const MODE_HINTS = {
  honest: "Holds the whole buffer in RAM and mlock()s it. The baseline every other row is measured against.",
  mmapdisk: "Keeps the data on disk and reads it through a memory-mapped view — no syscall per probe. Fast while it has the RAM to cache the file; give it less and watch.",
  disk: "Keeps the data on disk and pread()s every probe. Slow, but experiment 1 shows most of that is syscall overhead, not the disk.",
  partial: "Keeps half the buffer in RAM, the rest on disk. Correct answers, bimodal latency.",
  fake: "Keeps half the buffer and fabricates the rest. Fast and wrong — this is the one the correctness check catches, not the clock.",
};

const REDUCED_MOTION = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const $ = (id) => document.getElementById(id);
const el = {
  mode: $('mode'), mem: $('mem'), memLabel: $('memLabel'), memHint: $('memHint'),
  modeHint: $('modeHint'), mb: $('mb'), k: $('k'), n: $('n'), warmup: $('warmup'),
  runBtn: $('runBtn'), stopBtn: $('stopBtn'), clearBtn: $('clearBtn'), themeBtn: $('themeBtn'),
  led: $('led'), statusText: $('statusText'),
  latency: $('latency'), pagemap: $('pagemap'), collapse: $('collapse'),
  latLegend: $('latLegend'), mapLegend: $('mapLegend'), mapHint: $('mapHint'),
  mapScale: $('mapScale'), collapseLegend: $('collapseLegend'), consoleBody: $('consoleBody'), log: $('log'), tip: $('tip'),
  verdictState: $('verdictState'), heroRatio: $('heroRatio'), heroLabel: $('heroLabel'),
  heroSub: $('heroSub'),
  tP50: $('tP50'), tP50sub: $('tP50sub'), tErr: $('tErr'), tErrsub: $('tErrsub'),
  tStorage: $('tStorage'), tStoragesub: $('tStoragesub'), tRes: $('tRes'), tRessub: $('tRessub'),
};

const state = {
  run: null,        // current run
  prev: null,       // last completed run, kept on the chart for comparison
  baseline: null,   // last completed honest run: {label, p50}
  collapse: [],     // one point per completed run
  pending: [],      // challenge events waiting to be drawn (paced)
  dirty: { lat: true, map: true, collapse: true },
  hover: { lat: null, map: null },
};

// ── helpers ──────────────────────────────────────────────────────────────────
const fmtUs = (us) =>
  us == null ? '—' :
  us < 1000 ? `${us.toFixed(1)} µs` :
  us < 1e6 ? `${(us / 1000).toFixed(us < 1e4 ? 2 : 1)} ms` : `${(us / 1e6).toFixed(2)} s`;
const fmtRatio = (r) => (r < 10 ? `${r.toFixed(2)}×` : `${Math.round(r).toLocaleString()}×`);
const fmtMiB = (mib) => (mib >= 1024 ? `${(mib / 1024).toFixed(mib % 1024 ? 1 : 0)} GiB` : `${mib} MiB`);

function median(sorted) {
  if (!sorted.length) return null;
  const m = sorted.length >> 1;
  return sorted.length % 2 ? sorted[m] : (sorted[m - 1] + sorted[m]) / 2;
}

/** Decode a base64 bitstream (LSB-first within each byte) into one byte per flag. */
function bitsFromB64(b64, count) {
  const bin = atob(b64);
  const out = new Uint8Array(count);
  for (let i = 0; i < count; i++) {
    const byte = bin.charCodeAt(i >> 3);
    if (byte === undefined || Number.isNaN(byte)) break;
    out[i] = (byte >> (i & 7)) & 1;
  }
  return out;
}

function cssColor(name) {
  return getComputedStyle(document.body).getPropertyValue(name).trim();
}
function rgb(hex) {
  const h = hex.replace('#', '');
  const v = h.length === 3 ? h.split('').map((c) => c + c).join('') : h;
  return [parseInt(v.slice(0, 2), 16), parseInt(v.slice(2, 4), 16), parseInt(v.slice(4, 6), 16)];
}
const mix = (a, b, t) => a.map((x, i) => Math.round(x + (b[i] - x) * t));

/** Size a canvas for the device pixel ratio and return its 2D context. */
function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || 600;
  // The CSS height is authoritative. Never read back canvas.height: assigning it
  // rewrites the attribute, so the next read would compound.
  const h = canvas.clientHeight || parseInt(canvas.getAttribute('height'), 10) || 240;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

// ── run bookkeeping ──────────────────────────────────────────────────────────
function runLabel(cfg) { return `${cfg.mode} · ${cfg.mem.replace('m', ' MiB').replace('g', ' GiB')}`; }

function newRun(meta, cfg) {
  return {
    cfg, label: runLabel(cfg), meta,
    pages: meta.pages, pageBytes: meta.page_bytes,
    points: [], sortedRun: [], errors: 0, measured: 0,
    probe: new Uint8Array(meta.pages),      // probed pages, latest sampled challenge
    heat: new Float32Array(meta.pages),     // decaying "recently probed" glow
    heatFrames: 0,
    residency: null,                        // prover's mincore view
    mappedPages: null,                      // pages it can even inspect (0 for a pread reader)
    prover: null, p50: null, isHonest: cfg.mode === 'honest',
  };
}

function ingestChallenge(ev) {
  const run = state.run;
  if (!run) return;
  run.points.push(ev);
  if (ev.ph === 'r') {
    run.measured++;
    if (!ev.ok) run.errors++;
    // Sorted insert keeps the running median cheap enough to update every frame.
    const arr = run.sortedRun;
    let lo = 0, hi = arr.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (arr[mid] < ev.us) lo = mid + 1; else hi = mid; }
    arr.splice(lo, 0, ev.us);
    run.p50 = median(arr);
  }
  if (ev.map) {
    run.probe = bitsFromB64(ev.map, run.pages);
    if (REDUCED_MOTION) run.heat.fill(0);
    // A probe happens and is over; the glow decays so the eye can see *where* the
    // last sampled challenge landed without freezing it into a permanent state.
    for (let i = 0; i < run.pages; i++) if (run.probe[i]) run.heat[i] = 1;
    run.heatFrames = REDUCED_MOTION ? 0 : 110;
  }
  appendConsole(ev);
  state.dirty.lat = state.dirty.map = true;
}

function ingestProver(ev) {
  const run = state.run;
  if (!run) return;
  run.prover = ev;
  run.mappedPages = ev.pages;
  run.residency = ev.res ? bitsFromB64(ev.res, ev.pages) : null;
  state.dirty.map = true;
}

function finishRun(endEv) {
  const run = state.run;
  if (!run) return;
  if (endEv) { run.p50 = endEv.p50; run.errors = endEv.errors; run.measured = endEv.samples; }
  const mib = memMiB(run.cfg.mem);
  state.collapse.push({ mib, p50: run.p50, mode: run.cfg.mode, label: run.label,
                        errors: run.errors, measured: run.measured });
  if (run.isHonest && run.p50) state.baseline = { label: run.label, p50: run.p50 };
  state.prev = run;
  state.dirty.collapse = state.dirty.lat = true;
}

const memMiB = (mem) => (mem.endsWith('g') ? parseInt(mem, 10) * 1024 : parseInt(mem, 10));

// ── event stream ─────────────────────────────────────────────────────────────
function connect() {
  const es = new EventSource('/events');
  es.onmessage = (msg) => {
    let ev;
    try { ev = JSON.parse(msg.data); } catch { return; }
    switch (ev.t) {
      // Everything on the data timeline goes through ONE ordered queue. Applying
      // 'run' or 'p' immediately while 'c' waited its turn meant a page pacing
      // behind reality filed the previous run's challenges under the next run —
      // which silently lost the honest baseline.
      case 'run': case 'c': case 'p': case 'end':
        state.pending.push(ev); break;
      case 'history': restoreHistory(ev.runs); break;
      case 'status': onStatus(ev); break;
      case 'log':    appendLog(`[${ev.src}] ${ev.line}`); break;
    }
  };
  es.onerror = () => setStatus('err', 'bridge disconnected — is bridge.py still running?');
}

/** Rebuild baseline + collapse marks from runs that finished before this page
 *  connected, so a reload mid-demo doesn't lose the reference run. */
function restoreHistory(runs) {
  state.collapse = [];
  state.baseline = null;
  for (const r of runs || []) {
    if (r.p50 == null) continue;
    state.collapse.push({ mib: memMiB(r.cfg.mem), p50: r.p50, mode: r.cfg.mode,
                          label: runLabel(r.cfg), errors: r.errors, measured: r.samples });
    if (r.cfg.mode === 'honest') state.baseline = { label: runLabel(r.cfg), p50: r.p50 };
  }
  state.dirty.collapse = true;
  updateTiles();
}

function onStatus(ev) {
  const phase = ev.phase;
  const busy = ['building', 'starting prover', 'running'].includes(phase);
  el.runBtn.disabled = busy;
  if (phase === 'running') setStatus('busy', `running ${runLabel(ev.cfg)}`);
  else if (busy) setStatus('busy', phase);
  else if (phase === 'error') { setStatus('err', ev.error || 'error'); appendLog(`[error] ${ev.error}`); }
  else if (phase === 'done') setStatus(ev.oom ? 'err' : 'done', ev.oom ? 'prover OOM-killed — raise its RAM budget' : 'done');
  else setStatus('', phase);
}

function setStatus(cls, text) {
  el.led.className = `led ${cls}`;
  el.statusText.textContent = text;
}

// ── paced playback: drain the queue so a burst is watchable ──────────────────
/** Ingest queued challenges. Runs on an interval, not on a frame: a tab that is
 *  hidden or not painting stops getting frames, and the run must still be
 *  accounted for — otherwise switching away mid-demo silently stalls the page and
 *  dumps a backlog when you switch back. */
function apply(ev) {
  switch (ev.t) {
    case 'run': state.run = newRun(ev, ev.cfg || currentCfg()); clearConsole(); break;
    case 'c':   ingestChallenge(ev); break;
    case 'p':   ingestProver(ev); break;
    case 'end': finishRun(ev); break;
  }
}

function pump() {
  if (!state.pending.length) return;
  // Pacing is for the run being watched. If a newer run has already started,
  // fast-forward through the older one so the page never narrates stale work.
  const nextRunAt = state.pending.findIndex((e, i) => i > 0 && e.t === 'run');
  const budget = nextRunAt > 0 ? nextRunAt + 1
                               : Math.max(1, Math.ceil(state.pending.length / 12));
  for (let i = 0; i < budget && state.pending.length; i++) apply(state.pending.shift());
  updateTiles();
  // Frames can stop entirely (a tab that isn't painting), which would leave the
  // canvases showing an older run than the numbers beside them. Draw from here if
  // no frame has arrived recently.
  if (performance.now() - lastFrameAt > 500) redrawDirty();
}

let lastFrameAt = 0;

function redrawDirty() {
  if (state.dirty.lat) { drawLatency(); state.dirty.lat = false; }
  if (state.dirty.map) { drawPageMap(); state.dirty.map = false; }
  if (state.dirty.collapse) { drawCollapse(); state.dirty.collapse = false; }
}

function tick() {
  lastFrameAt = performance.now();
  const run = state.run;
  if (run && run.heatFrames > 0) {
    for (let i = 0; i < run.pages; i++) run.heat[i] *= 0.93;
    run.heatFrames--;
    state.dirty.map = true;
  }
  redrawDirty();
  requestAnimationFrame(tick);
}

// ── readouts + verdict ──────────────────────────────────────────────────────
function updateTiles() {
  const run = state.run;
  const p = run && run.prover;
  if (!run && state.collapse.length) return summarizeLastRun();

  el.tP50.textContent = run && run.p50 != null ? fmtUs(run.p50) : '—';
  el.tP50sub.textContent = run ? `${run.measured} of ${run.cfg.n} measured` : 'measured phase';

  if (run && run.measured) {
    el.tErr.textContent = `${run.errors.toLocaleString()} / ${run.measured.toLocaleString()}`;
    el.tErr.className = run.errors ? 'num alarm' : 'num';
    el.tErrsub.textContent = run.errors ? 'wrong, however fast' : 'all matched';
  } else { el.tErr.textContent = '—'; el.tErr.className = 'num'; el.tErrsub.textContent = 'vs local recompute'; }

  const sp = storagePct(p);
  if (sp != null) {
    el.tStorage.textContent = `${sp < 10 ? sp.toFixed(1) : Math.round(sp)}%`;
    el.tStoragesub.textContent = `${p.majflt.toLocaleString()} major faults`;
  } else { el.tStorage.textContent = '—'; el.tStoragesub.textContent = 'block-layer reads'; }

  const rp = residentPct(p);
  if (rp != null) {
    el.tRes.textContent = `${rp < 10 ? rp.toFixed(1) : Math.round(rp)}%`;
    el.tRessub.textContent = p.pages === 0 ? 'maps no pages at all'
      : `${p.res_pages.toLocaleString()} of ${p.pages_total.toLocaleString()} pages`;
  } else { el.tRes.textContent = '—'; el.tRessub.textContent = 'kernel mincore'; }

  updateVerdict();
}

/** With no live run, say what the last finished one measured rather than showing
 *  a row of dashes — but keep it labelled as a summary, not a live reading. */
function summarizeLastRun() {
  const last = state.collapse[state.collapse.length - 1];
  el.tP50.textContent = fmtUs(last.p50);
  el.tP50sub.textContent = `${last.label}, ${last.measured} challenges`;
  el.tErr.textContent = `${last.errors.toLocaleString()} / ${last.measured.toLocaleString()}`;
  el.tErr.className = last.errors ? 'num alarm' : 'num';
  el.tErrsub.textContent = last.errors ? 'wrong, however fast' : 'all matched';
  el.tStorage.textContent = '—';
  el.tStoragesub.textContent = 'no live run';
  el.tRes.textContent = '—';
  el.tRessub.textContent = 'no live run';
  el.verdictState.className = 'state';
  el.verdictState.textContent = `last run · ${last.label}`;
  if (state.baseline && last.p50) {
    el.heroRatio.textContent = fmtRatio(last.p50 / state.baseline.p50);
    el.heroLabel.textContent = last.p50 / state.baseline.p50 >= 1.5
      ? 'slower than an honest prover' : 'as fast as an honest prover';
    el.heroSub.textContent = `Finished run, not live: p50 ${fmtUs(last.p50)} against ${state.baseline.label} at ${fmtUs(state.baseline.p50)}.`;
  } else {
    el.heroRatio.textContent = fmtUs(last.p50);
    el.heroLabel.textContent = 'last finished run · p50';
    el.heroSub.textContent = 'Run the honest prover to get a reference for this session.';
  }
}

/** Share of probes that had to come from storage: the prover's own block-layer
 *  reads over the probes it was asked for. Defined for every reader — an mmap
 *  probe pays a major fault and a pread probe does not, but both show up here. */
function storagePct(p) {
  if (!p || !p.probes || p.read_bytes < 0) return null;
  return 100 * (p.read_bytes / 4096) / p.probes;
}
/** Share of the claimed buffer the kernel says is resident. A pread reader maps
 *  nothing, so this is 0 by construction — true, but the storage share is the
 *  honest signal for that reader, not this one. */
function residentPct(p) {
  if (!p || !p.pages_total) return null;
  return 100 * p.res_pages / p.pages_total;
}

function updateVerdict() {
  const run = state.run, p = run && run.prover;
  const ratio = run && run.p50 != null && state.baseline ? run.p50 / state.baseline.p50 : null;

  if (ratio != null) {
    el.heroRatio.textContent = fmtRatio(ratio);
    el.heroLabel.textContent = ratio >= 1.5 ? 'slower than an honest prover'
      : ratio <= 0.85 ? 'faster than honest — check the answers'
      : 'as fast as an honest prover';
    el.heroSub.textContent = `p50 ${fmtUs(run.p50)} against ${state.baseline.label} at ${fmtUs(state.baseline.p50)}, same session.`;
  } else if (run && run.p50 != null) {
    el.heroRatio.textContent = fmtUs(run.p50);
    el.heroLabel.textContent = 'p50 · no honest baseline yet';
    el.heroSub.textContent = 'Run the honest prover to get a reference for this session — absolute microseconds do not travel between sessions.';
  } else {
    el.heroRatio.textContent = '—';
    el.heroLabel.textContent = 'press Interrogate';
    el.heroSub.textContent = 'Run the honest prover first — it is the reference every other prover is measured against.';
  }

  // The state line is only ever a restatement of measured counters, and it names
  // which counter it rests on. Icon + words carry it; colour only agrees.
  let cls = '', text = 'standing by';
  if (run) {
    const sp = storagePct(p), rp = residentPct(p);
    const maps = p ? p.pages > 0 : true;
    if (run.errors > 0) {
      cls = 'wrong';
      text = `✕ wrong answers — ${run.errors} of ${run.measured}`;
    } else if (sp == null) {
      text = 'measuring…';
    } else if (sp > 20) {
      cls = 'storage';
      text = `not holding it — ${Math.round(sp)}% of probes reach storage`;
    } else if (!maps) {
      cls = 'holding';
      text = 'data is cached, but every probe pays a syscall';
    } else if (rp != null && rp > 95) {
      cls = 'holding';
      text = 'holding the data in fast memory';
    } else {
      cls = 'storage';
      text = `holding ${Math.round(rp)}% of what it claims`;
    }
  }
  el.verdictState.className = `state ${cls}`;
  el.verdictState.textContent = text;
}

// ── latency chart ────────────────────────────────────────────────────────────
function seriesColor(run) { return cssColor(run.isHonest ? '--fast' : '--slow'); }

function latencyGeom(w, h, runs) {
  const pad = { l: 54, r: 16, t: 12, b: 26 };
  let lo = Infinity, hi = -Infinity, maxX = 1;
  for (const r of runs) {
    for (const pt of r.points) { if (pt.us < lo) lo = pt.us; if (pt.us > hi) hi = pt.us; }
    maxX = Math.max(maxX, r.cfg.warmup + r.cfg.n, r.points.length);
  }
  if (!Number.isFinite(lo)) { lo = 10; hi = 1000; }
  let yLo = Math.log10(Math.max(1, lo / 1.6)), yHi = Math.log10(hi * 1.6);
  if (yHi - yLo < 1) {                                 // never less than one decade
    const mid = (yLo + yHi) / 2;
    yLo = mid - 0.5; yHi = mid + 0.5;
  }
  const span = yHi - yLo;
  return {
    pad, maxX, yLo, yHi,
    x: (i) => pad.l + (w - pad.l - pad.r) * (i / maxX),
    y: (us) => h - pad.b - (h - pad.t - pad.b) * ((Math.log10(Math.max(us, 0.1)) - yLo) / span),
  };
}

function drawLatency() {
  const { ctx, w, h } = fitCanvas(el.latency);
  const runs = [state.prev, state.run].filter(Boolean).filter((r) => r.points.length);
  renderLatLegend(runs);
  if (!runs.length) {
    emptyNote(ctx, w, h, state.collapse.length
      ? 'no live run — marks from finished runs are in "where it breaks"'
      : 'press Interrogate — challenges will stream in here');
    return;
  }
  const g = latencyGeom(w, h, runs);

  // warmup region: unmeasured challenges, shown but set apart
  const wu = runs[runs.length - 1].cfg.warmup;
  if (wu > 0) {
    ctx.fillStyle = cssColor('--hair');
    ctx.globalAlpha = 0.5;
    ctx.fillRect(g.pad.l, g.pad.t, g.x(wu) - g.pad.l, h - g.pad.t - g.pad.b);
    ctx.globalAlpha = 1;
    ctx.fillStyle = cssColor('--muted');
    ctx.font = `10px ${cssColor('--face-num')}`;
    ctx.fillText('warmup', g.pad.l + 3, g.pad.t + 11);
  }

  // decade gridlines — solid hairlines, one shade off the surface
  ctx.strokeStyle = cssColor('--hair');
  ctx.fillStyle = cssColor('--muted');
  ctx.lineWidth = 1;
  ctx.font = `10px ${cssColor('--face-num')}`;
  ctx.textAlign = 'right';
  // Decades, plus the 2x/5x steps inside them when the run spans less than ~1.5
  // decades — otherwise a tight distribution gets a single lonely gridline.
  const steps = (g.yHi - g.yLo) < 1.6 ? [1, 2, 5] : [1];
  for (let d = Math.floor(g.yLo); d <= Math.ceil(g.yHi); d++) {
    for (const mult of steps) {
      const us = Math.pow(10, d) * mult, y = g.y(us);
      if (y < g.pad.t - 1 || y > h - g.pad.b + 1) continue;
      ctx.strokeStyle = cssColor('--hair');
      ctx.beginPath(); ctx.moveTo(g.pad.l, y); ctx.lineTo(w - g.pad.r, y); ctx.stroke();
      ctx.fillText(fmtUs(us), g.pad.l - 6, y + 3);
    }
  }
  ctx.textAlign = 'left';
  ctx.strokeStyle = cssColor('--axis');
  ctx.beginPath(); ctx.moveTo(g.pad.l, h - g.pad.b); ctx.lineTo(w - g.pad.r, h - g.pad.b); ctx.stroke();
  ctx.fillText(`challenge # (0 … ${g.maxX.toLocaleString()})`, g.pad.l, h - 8);

  // series: the retained run is thinner and translucent, and labelled "previous",
  // so two runs of the same class never rely on hue alone to be told apart.
  runs.forEach((run) => {
    const isCurrent = run === state.run;
    ctx.strokeStyle = seriesColor(run);
    ctx.globalAlpha = isCurrent ? 1 : 0.55;
    ctx.lineWidth = isCurrent ? 2 : 1.25;
    ctx.beginPath();
    run.points.forEach((pt, i) => {
      const x = g.x(i), y = g.y(pt.us);
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.stroke();
    if (run.points.length <= 60) {                    // few enough to mark individually
      ctx.fillStyle = seriesColor(run);
      run.points.forEach((pt, i) => {
        ctx.beginPath(); ctx.arc(g.x(i), g.y(pt.us), 4, 0, Math.PI * 2); ctx.fill();
      });
    }
    // direct label at the series end
    const last = run.points[run.points.length - 1];
    ctx.fillStyle = seriesColor(run);
    ctx.font = `${isCurrent ? '600 ' : ''}10px ${cssColor('--face-label')}`;
    const lx = Math.min(g.x(run.points.length - 1) + 6, w - g.pad.r - 4);
    ctx.textAlign = g.x(run.points.length - 1) > w - 120 ? 'right' : 'left';
    ctx.fillText(`${run.label}${isCurrent ? '' : ' (previous)'}`, lx, g.y(last.us) - 7);
    ctx.textAlign = 'left';
    ctx.globalAlpha = 1;
  });

  drawLatHover(ctx, w, h, g, runs);
}

function drawLatHover(ctx, w, h, g, runs) {
  const hv = state.hover.lat;
  if (!hv) return;
  const run = state.run && state.run.points.length ? state.run : runs[runs.length - 1];
  const idx = Math.round((hv.x - g.pad.l) / (w - g.pad.l - g.pad.r) * g.maxX);
  const pt = run.points[Math.max(0, Math.min(run.points.length - 1, idx))];
  if (!pt) return;
  const x = g.x(run.points.indexOf(pt));
  ctx.strokeStyle = cssColor('--axis');
  ctx.beginPath(); ctx.moveTo(x, g.pad.t); ctx.lineTo(x, h - g.pad.b); ctx.stroke();
  ctx.fillStyle = seriesColor(run);
  ctx.beginPath(); ctx.arc(x, g.y(pt.us), 4.5, 0, Math.PI * 2); ctx.fill();
  ctx.strokeStyle = cssColor('--panel'); ctx.lineWidth = 2; ctx.stroke();
  showTip(hv.clientX, hv.clientY,
    `<b>${fmtUs(pt.us)}</b><br><span class="k">challenge</span> #${pt.i} `
    + `${pt.ph === 'w' ? '(warmup)' : ''}<br><span class="k">verdict</span> `
    + `${pt.ok ? '✓ match' : '✕ mismatch'}<br><span class="k">run</span> ${run.label}`);
}

function renderLatLegend(runs) {
  if (!runs.length) { el.latLegend.innerHTML = ''; return; }
  el.latLegend.innerHTML = runs.map((r) => {
    const c = seriesColor(r);
    const current = r === state.run;
    // Two runs of the same class share a hue, so the swatch carries the same
    // opacity the mark does — the legend must not be the only place they differ.
    const swatch = `<i style="opacity:${current ? 1 : 0.55}"></i>`;
    const badge = r.errors ? `<span class="badge">✕ ${r.errors}/${r.measured} mismatched</span>` : '';
    return `<span style="color:${c}">${swatch}<span style="color:var(--ink-2)">${r.label}`
         + `${current ? '' : ' (previous)'}</span></span>${badge}`;
  }).join('');
}

// ── page map ─────────────────────────────────────────────────────────────────
function drawPageMap() {
  const { ctx, w, h } = fitCanvas(el.pagemap);
  const run = state.run || state.prev;
  renderMapLegend(run);
  if (!run) {
    emptyNote(ctx, w, h, state.collapse.length
      ? 'no live run — press Interrogate to map the buffer again'
      : 'the buffer will appear here, one cell per 4 KiB page');
    return;
  }

  const pages = run.pages;
  // Shape the grid to the panel's aspect so the buffer fills the screen rather
  // than sitting in a square island with dead space either side.
  const aspect = Math.max(0.4, w / h);
  const cols = Math.max(1, Math.round(Math.sqrt(pages * aspect)));
  const rows = Math.ceil(pages / cols);
  const cell = Math.max(1, Math.floor(Math.min(w / cols, h / rows)));
  const gw = cols * cell, gh = rows * cell;
  const ox = Math.floor((w - gw) / 2), oy = Math.floor((h - gh) / 2);

  const surface = rgb(cssColor('--inset'));
  const fast = rgb(cssColor('--fast'));
  const slow = rgb(cssColor('--slow'));
  const grid = rgb(cssColor('--hair'));
  const muted = rgb(cssColor('--muted'));
  const PAL = {
    probedFast: fast,
    probedSlow: slow,
    restFast: mix(surface, fast, 0.30),
    restCold: mix(surface, grid, 0.95),
    unmapped: mix(surface, muted, 0.20),
  };

  // One pixel per page into an offscreen image, then scaled up with smoothing
  // off: 16k pages redraw every frame without 16k fill calls.
  const img = ctx.createImageData(cols, rows);
  const mapped = run.mappedPages;
  for (let i = 0; i < pages; i++) {
    // Resting colour says whether the page is held; the glow says a probe just
    // landed there. A probe is momentary, so it is drawn as a momentary thing.
    let base, lit;
    if (mapped === 0 || (mapped != null && i >= mapped)) { base = PAL.unmapped; lit = PAL.probedSlow; }
    else if (!run.residency || run.residency[i]) { base = PAL.restFast; lit = PAL.probedFast; }
    else { base = PAL.restCold; lit = PAL.probedSlow; }
    const t = run.heat ? Math.min(1, run.heat[i]) : 0;
    const c = t > 0.015 ? mix(base, lit, t) : base;
    const o = i * 4;
    img.data[o] = c[0]; img.data[o + 1] = c[1]; img.data[o + 2] = c[2]; img.data[o + 3] = 255;
  }
  const off = document.createElement('canvas');
  off.width = cols; off.height = rows;
  off.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(off, ox, oy, gw, gh);

  el.pagemap.__geom = { ox, oy, cell, cols, rows, pages, run };
  el.mapScale.textContent =
    `${pages.toLocaleString()} pages · ${(run.pageBytes / 1024).toFixed(0)} KiB each · ${run.cfg.mb} MiB claimed`;
  drawMapHover(ctx);
}

function drawMapHover(ctx) {
  const hv = state.hover.map, g = el.pagemap.__geom;
  if (!hv || !g) return;
  const cx = Math.floor((hv.x - g.ox) / g.cell), cy = Math.floor((hv.y - g.oy) / g.cell);
  if (cx < 0 || cy < 0 || cx >= g.cols || cy >= g.rows) return hideTip();
  const page = cy * g.cols + cx;
  if (page >= g.pages) return hideTip();
  const run = g.run;
  const mapped = run.mappedPages;
  const held = mapped === 0 ? false : (mapped != null && page >= mapped ? false
              : (run.residency ? !!run.residency[page] : true));
  ctx.strokeStyle = cssColor('--ink');
  ctx.lineWidth = 1.5;
  ctx.strokeRect(g.ox + cx * g.cell - 1, g.oy + cy * g.cell - 1, g.cell + 2, g.cell + 2);
  const off = page * run.pageBytes;
  showTip(hv.clientX, hv.clientY,
    `<b>page ${page.toLocaleString()}</b><br>`
    + `<span class="k">offset</span> ${(off / 1048576).toFixed(2)} MiB<br>`
    + `<span class="k">state</span> ${mapped === 0 ? 'not held (syscall reader)' : held ? 'resident in memory' : 'evicted — a probe here faults to storage'}<br>`
    + `<span class="k">this challenge</span> ${run.probe && run.probe[page] ? 'probed' : 'not probed'}`);
}

function renderMapLegend(run) {
  const items = [
    ['--fast', 'probed · came from memory'],
    ['--slow', 'probed · came from storage'],
    ['--hair', 'evicted — a probe here faults'],
    ['--muted', 'not held at all'],
  ];
  el.mapLegend.innerHTML = items.map(([v, label]) =>
    `<span style="color:${cssColor(v)}"><i></i><span style="color:var(--ink-2)">${label}</span></span>`).join('');
  if (!run) return;
  const p = run.prover;
  el.mapHint.textContent = p && p.pages === 0
    ? `This prover maps nothing: it holds 0 of ${p.pages_total.toLocaleString()} pages and pays a pread() syscall for every probe.`
    : p
      ? `${p.res_pages.toLocaleString()} of ${p.pages_total.toLocaleString()} pages resident, from the kernel's mincore snapshot taken every ${run.cfg.trace} challenges; probe positions come from the verifier's own index sequence for the latest sampled challenge. The two are sampled separately, so they can be up to ${run.cfg.trace} challenges apart.`
      : 'Every cell is one 4 KiB page. Colour is where the probes actually came from, per the kernel — not a claim.';
}

// ── collapse curve ───────────────────────────────────────────────────────────
function drawCollapse() {
  const { ctx, w, h } = fitCanvas(el.collapse);
  const pts = state.collapse.filter((p) => p.p50 != null);
  renderCollapseLegend(pts);
  if (!pts.length) { emptyNote(ctx, w, h, 'completed runs land here'); return; }
  const pad = { l: 58, r: 16, t: 12, b: 22 };
  const xs = pts.map((p) => Math.log10(p.mib));
  const ys = pts.map((p) => Math.log10(p.p50));
  const xLo = Math.min(...xs) - 0.12, xHi = Math.max(...xs) + 0.12;
  const yLo = Math.min(...ys) - 0.15, yHi = Math.max(...ys) + 0.15;
  const X = (v) => pad.l + (w - pad.l - pad.r) * ((v - xLo) / Math.max(xHi - xLo, 0.3));
  const Y = (v) => h - pad.b - (h - pad.t - pad.b) * ((v - yLo) / Math.max(yHi - yLo, 0.3));

  ctx.strokeStyle = cssColor('--hair');
  ctx.fillStyle = cssColor('--muted');
  ctx.font = `9px ${cssColor('--face-num')}`;
  ctx.textAlign = 'right';
  for (let d = Math.floor(yLo); d <= Math.ceil(yHi); d++) {
    const y = Y(d);
    if (y < pad.t || y > h - pad.b) continue;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
    ctx.fillText(fmtUs(Math.pow(10, d)), pad.l - 5, y + 3);
  }
  ctx.textAlign = 'center';
  ctx.fillText('prover RAM budget →', (pad.l + w) / 2, h - 6);

  for (const honest of [true, false]) {
    const group = pts.filter((p) => (p.mode === 'honest') === honest).sort((a, b) => a.mib - b.mib);
    if (!group.length) continue;
    const color = cssColor(honest ? '--fast' : '--slow');
    ctx.strokeStyle = color; ctx.fillStyle = color; ctx.lineWidth = 2;
    // Runs at the same budget would sit on top of each other; nudge them apart by
    // a couple of pixels so a hidden mark can't read as a missing one.
    const dodge = (p) => {
      const same = pts.filter((q) => q.mib === p.mib);
      const i = same.indexOf(p);
      return same.length > 1 ? (i - (same.length - 1) / 2) * 5 : 0;
    };
    ctx.beginPath();
    group.forEach((p, i) => {
      const x = X(Math.log10(p.mib)) + dodge(p), y = Y(Math.log10(p.p50));
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.stroke();
    group.forEach((p) => {
      const x = X(Math.log10(p.mib)) + dodge(p), y = Y(Math.log10(p.p50));
      ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2); ctx.fill();
      ctx.strokeStyle = cssColor('--panel'); ctx.lineWidth = 2; ctx.stroke();
      ctx.strokeStyle = color; ctx.lineWidth = 2;
    });
  }

  // Label the budgets, once each: runs at the same budget would otherwise stack
  // identical labels on top of each other.
  ctx.fillStyle = cssColor('--muted');
  ctx.font = `9px ${cssColor('--face-num')}`;
  ctx.textAlign = 'center';
  const labelled = new Set();
  for (const p of [...pts].sort((a, b) => a.mib - b.mib)) {
    if (labelled.has(p.mib)) continue;
    labelled.add(p.mib);
    const sameBudget = pts.filter((q) => q.mib === p.mib);
    const topY = Math.min(...sameBudget.map((q) => Y(Math.log10(q.p50))));
    ctx.fillText(fmtMiB(p.mib), X(Math.log10(p.mib)), topY - 9);
  }
}

function renderCollapseLegend(pts) {
  if (!el.collapseLegend) return;
  const kinds = [];
  if (pts.some((p) => p.mode === 'honest')) kinds.push(['--fast', 'honest prover']);
  if (pts.some((p) => p.mode !== 'honest')) kinds.push(['--slow', 'cheating prover']);
  el.collapseLegend.innerHTML = kinds.map(([v, label]) =>
    `<span style="color:${cssColor(v)}"><i></i><span style="color:var(--ink-2)">${label}</span></span>`).join('');
}

// ── console ──────────────────────────────────────────────────────────────────
const CONSOLE_MAX = 240;
function clearConsole() { el.consoleBody.innerHTML = ''; }

function appendConsole(ev) {
  const body = el.consoleBody;
  const atBottom = body.parentElement.scrollTop + body.parentElement.clientHeight
                   >= body.parentElement.scrollHeight - 24;
  const tr = document.createElement('tr');
  if (!ev.ok) tr.className = 'bad';
  else if (ev.ph === 'w') tr.className = 'warm';
  tr.innerHTML = `<td>${ev.i}</td><td>…${ev.nonce.slice(-9)}</td>`
    + `<td>${ev.hash.slice(0, 10)}…</td><td>${ev.us.toFixed(1)}</td>`
    + `<td class="${ev.ok ? 'v-ok' : 'v-bad'}">${ev.ok ? 'match' : 'MISMATCH'}</td>`;
  body.appendChild(tr);
  while (body.childElementCount > CONSOLE_MAX) body.removeChild(body.firstChild);
  if (atBottom) body.parentElement.scrollTop = body.parentElement.scrollHeight;
}

function appendLog(line) {
  el.log.textContent += line + '\n';
  const lines = el.log.textContent.split('\n');
  if (lines.length > 400) el.log.textContent = lines.slice(-400).join('\n');
  el.log.scrollTop = el.log.scrollHeight;
}

// ── shared chart bits ────────────────────────────────────────────────────────
function emptyNote(ctx, w, h, text) {
  ctx.fillStyle = cssColor('--muted');
  ctx.font = `12px ${cssColor('--face-prose')}`;
  ctx.textAlign = 'center';
  ctx.fillText(text, w / 2, h / 2);
  ctx.textAlign = 'left';
}
function showTip(cx, cy, html) {
  el.tip.innerHTML = html;
  el.tip.hidden = false;
  const r = el.tip.getBoundingClientRect();
  el.tip.style.left = `${Math.min(cx + 14, window.innerWidth - r.width - 8)}px`;
  el.tip.style.top = `${Math.max(8, Math.min(cy + 14, window.innerHeight - r.height - 8))}px`;
}
function hideTip() { el.tip.hidden = true; }

function trackHover(canvas, key) {
  canvas.addEventListener('pointermove', (e) => {
    const r = canvas.getBoundingClientRect();
    state.hover[key] = { x: e.clientX - r.left, y: e.clientY - r.top, clientX: e.clientX, clientY: e.clientY };
    state.dirty[key === 'lat' ? 'lat' : 'map'] = true;
  });
  canvas.addEventListener('pointerleave', () => {
    state.hover[key] = null; hideTip();
    state.dirty[key === 'lat' ? 'lat' : 'map'] = true;
  });
}

// ── controls ─────────────────────────────────────────────────────────────────
function currentCfg() {
  const mib = MEM_STEPS[+el.mem.value];
  return {
    mode: el.mode.value,
    mem: mib >= 1024 && mib % 1024 === 0 ? `${mib / 1024}g` : `${mib}m`,
    mb: +el.mb.value, k: +el.k.value, lanes: 32,
    n: +el.n.value, warmup: +el.warmup.value,
    trace: 20, probe: 20, frac: 0.5,
  };
}

function syncControls() {
  const mib = MEM_STEPS[+el.mem.value];
  const buf = +el.mb.value;
  el.memLabel.textContent = fmtMiB(mib);
  const ratio = mib / buf;
  el.memHint.textContent = ratio >= 1.5
    ? `${ratio.toFixed(2)}× the buffer it claims to hold — room to cache the whole thing, so an mmap reader stays fast.`
    : ratio >= 1
      ? `${ratio.toFixed(2)}× the buffer — barely enough; expect it to hold most of the file.`
      : `${ratio.toFixed(2)}× the buffer — the page cache CANNOT keep the file. Probes have to fault to storage.`;
  el.modeHint.textContent = MODE_HINTS[el.mode.value] || '';
  if (el.mode.value === 'honest' && ratio < 1.2) {
    el.memHint.textContent += ' An honest prover needs the RAM: below ~1.2× it gets OOM-killed.';
  }
}

el.runBtn.addEventListener('click', async () => {
  const cfg = currentCfg();
  state.pendingCfg = cfg;
  el.runBtn.disabled = true;
  setStatus('busy', 'starting');
  try {
    await fetch('/run', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                          body: JSON.stringify(cfg) });
  } catch { setStatus('err', 'could not reach the bridge'); el.runBtn.disabled = false; }
});
el.stopBtn.addEventListener('click', () => fetch('/stop', { method: 'POST' }).catch(() => {}));
el.clearBtn.addEventListener('click', () => {
  state.collapse = []; state.prev = null; state.baseline = null;
  state.dirty.collapse = state.dirty.lat = true;
  updateTiles();
});
el.themeBtn.addEventListener('click', () => {
  const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = next;
  state.dirty.lat = state.dirty.map = state.dirty.collapse = true;
});
['input', 'change'].forEach((evt) => {
  el.mem.addEventListener(evt, syncControls);
  el.mode.addEventListener(evt, syncControls);
  el.mb.addEventListener(evt, syncControls);
});
window.addEventListener('resize', () => {
  state.dirty.lat = state.dirty.map = state.dirty.collapse = true;
});

setInterval(pump, 40);
trackHover(el.latency, 'lat');
trackHover(el.pagemap, 'map');
syncControls();
updateTiles();
connect();
requestAnimationFrame(tick);
