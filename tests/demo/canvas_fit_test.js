// Checks that the GPU demo's fit() keeps a canvas the same size across redraws.
'use strict';
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'demo', 'static', 'gpu.js'), 'utf8');
const m = src.match(/function fit\(c\) \{[\s\S]*?\n\}/);
if (!m) {
  console.error('FAIL: could not find fit() in gpu.js');
  process.exit(1);
}

// A canvas whose .height setter writes the height attribute, as in a browser.
function makeCanvas({ cssHeight = null, cssWidth = 800, attrHeight = 200 } = {}) {
  return {
    _attr: { height: String(attrHeight) },
    dataset: {},
    style: {},
    width: 0,
    get height() { return Number(this._attr.height); },
    set height(v) { this._attr.height = String(v); },
    getAttribute(n) { return this._attr[n]; },
    get clientWidth() { return cssWidth; },
    get clientHeight() { return cssHeight != null ? cssHeight : Number(this._attr.height); },
    getContext() { return { setTransform() {}, clearRect() {} }; },
  };
}

const fit = new Function('window', 'MAX_PX', `return (${m[0].replace(/^function fit/, 'function')});`)
  ({ devicePixelRatio: 2 }, 2000);

let failed = 0;
function check(name, cond, detail) {
  console.log(`${cond ? 'PASS' : 'FAIL'}: ${name}${detail ? '  (' + detail + ')' : ''}`);
  if (!cond) failed++;
}

{
  const c = makeCanvas({ cssHeight: 132, attrHeight: 132 });
  const seen = new Set();
  for (let i = 0; i < 50; i++) seen.add(fit(c).h);
  check('with a CSS height, logical height is stable over 50 redraws',
        seen.size === 1 && seen.has(132), `heights seen: ${[...seen].join(', ')}`);
  check('bitmap settles at CSS height x dpr', c.height === 264, `bitmap=${c.height}`);
}

{
  const c = makeCanvas({ cssHeight: null, attrHeight: 220 });
  const heights = [];
  for (let i = 0; i < 50; i++) heights.push(fit(c).h);
  const grew = heights[heights.length - 1] > heights[0];
  check('with NO CSS height, logical height never grows',
        !grew, `first=${heights[0]} last=${heights[heights.length - 1]}`);
  check('bitmap stays under the clamp', c.height <= 2000 * 2, `bitmap=${c.height}`);
}

{
  const c = makeCanvas({ cssHeight: 99999, attrHeight: 99999 });
  const r = fit(c);
  check('an absurd CSS height is clamped', r.h <= 2000, `h=${r.h}`);
}

console.log(failed === 0 ? '\ncanvas fit: all checks PASS'
                         : `\ncanvas fit: ${failed} check(s) FAILED`);
process.exit(failed === 0 ? 0 : 1);
