// Region-screenshot overlay. Renders the captured desktop full-screen, lets the
// user drag a rectangle, then crops that region to PNG and hands it to main for
// the clipboard. All selection coords are CSS pixels; we scale to the image's
// native (physical) pixels at crop time so DPI scaling is handled correctly.

const shot = document.getElementById('shot');
const sel = document.getElementById('sel');
const sizeLabel = document.getElementById('size');
const bar = document.getElementById('bar');
const hint = document.getElementById('hint');
const work = document.getElementById('work');
const maskTop = document.getElementById('maskTop');
const maskBottom = document.getElementById('maskBottom');
const maskLeft = document.getElementById('maskLeft');
const maskRight = document.getElementById('maskRight');

let imgMeta = null;        // { url, width, height } from main (native px)
let dragging = false;
let start = null;          // { x, y } drag origin
let rect = null;           // committed selection { x, y, w, h } in CSS px

window.overlayApi.onImage((data) => {
  imgMeta = data;
  shot.src = data.url;
});

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

function rectFromPoints(ax, ay, bx, by) {
  const x = Math.min(ax, bx);
  const y = Math.min(ay, by);
  return {
    x: clamp(x, 0, window.innerWidth),
    y: clamp(y, 0, window.innerHeight),
    w: Math.min(Math.abs(bx - ax), window.innerWidth - x),
    h: Math.min(Math.abs(by - ay), window.innerHeight - y),
  };
}

function layoutMasks(r) {
  const W = window.innerWidth, H = window.innerHeight;
  maskTop.style.cssText    = `left:0;top:0;width:${W}px;height:${r.y}px`;
  maskBottom.style.cssText = `left:0;top:${r.y + r.h}px;width:${W}px;height:${H - r.y - r.h}px`;
  maskLeft.style.cssText   = `left:0;top:${r.y}px;width:${r.x}px;height:${r.h}px`;
  maskRight.style.cssText  = `left:${r.x + r.w}px;top:${r.y}px;width:${W - r.x - r.w}px;height:${r.h}px`;
}

function drawSelection(r) {
  sel.style.display = 'block';
  sel.style.left = r.x + 'px';
  sel.style.top = r.y + 'px';
  sel.style.width = r.w + 'px';
  sel.style.height = r.h + 'px';
  layoutMasks(r);

  // Size readout in native pixels (what actually gets copied).
  const scaleX = imgMeta ? imgMeta.width / window.innerWidth : 1;
  const scaleY = imgMeta ? imgMeta.height / window.innerHeight : 1;
  sizeLabel.style.display = 'block';
  sizeLabel.textContent = `${Math.round(r.w * scaleX)} × ${Math.round(r.h * scaleY)}`;
  const labelTop = r.y > 26 ? r.y - 24 : r.y + 6;
  sizeLabel.style.left = r.x + 'px';
  sizeLabel.style.top = labelTop + 'px';
}

function showBar(r) {
  bar.style.display = 'flex';
  // Prefer below-right of the selection; flip up if it would overflow.
  let bx = r.x + r.w - 156;
  let by = r.y + r.h + 8;
  if (by + 44 > window.innerHeight) by = r.y + r.h - 44;
  bar.style.left = clamp(bx, 8, window.innerWidth - 164) + 'px';
  bar.style.top = clamp(by, 8, window.innerHeight - 48) + 'px';
}

function hideBar() { bar.style.display = 'none'; }

window.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return;            // left button only
  // Clicking the toolbar shouldn't start a new drag.
  if (bar.contains(e.target)) return;
  dragging = true;
  start = { x: e.clientX, y: e.clientY };
  rect = null;
  hint.style.display = 'none';
  hideBar();
  sel.style.display = 'none';
  sizeLabel.style.display = 'none';
});

window.addEventListener('mousemove', (e) => {
  if (!dragging) return;
  const r = rectFromPoints(start.x, start.y, e.clientX, e.clientY);
  rect = r;
  drawSelection(r);
});

window.addEventListener('mouseup', (e) => {
  if (!dragging) return;
  dragging = false;
  if (!rect || rect.w < 3 || rect.h < 3) {  // treated as a click, not a drag
    rect = null;
    sel.style.display = 'none';
    sizeLabel.style.display = 'none';
    for (const m of [maskTop, maskBottom, maskLeft, maskRight]) m.style.cssText = '';
    hint.style.display = 'block';
    return;
  }
  showBar(rect);
});

// Right-click or Esc cancels the whole screenshot.
window.addEventListener('contextmenu', (e) => { e.preventDefault(); window.overlayApi.cancel(); });
window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') window.overlayApi.cancel();
  else if (e.key === 'Enter') commit();
});

document.getElementById('cancel').addEventListener('click', () => window.overlayApi.cancel());
document.getElementById('ok').addEventListener('click', commit);

function commit() {
  if (!rect || !imgMeta || rect.w < 3 || rect.h < 3) return;
  const scaleX = imgMeta.width / window.innerWidth;
  const scaleY = imgMeta.height / window.innerHeight;
  const sx = Math.round(rect.x * scaleX);
  const sy = Math.round(rect.y * scaleY);
  const sw = Math.max(1, Math.round(rect.w * scaleX));
  const sh = Math.max(1, Math.round(rect.h * scaleY));

  work.width = sw;
  work.height = sh;
  const ctx = work.getContext('2d');
  ctx.drawImage(shot, sx, sy, sw, sh, 0, 0, sw, sh);

  work.toBlob((blob) => {
    blob.arrayBuffer().then((buf) => window.overlayApi.commit(buf, sw, sh));
  }, 'image/png');
}
