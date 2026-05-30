const btnShot = document.getElementById('btnShot');
const btnStream = document.getElementById('btnStream');
const statusEl = document.getElementById('status');
const img = document.getElementById('shot');
const canvas = document.getElementById('screen');
const placeholder = document.getElementById('placeholder');
const stats = document.getElementById('stats');
const stRes = document.getElementById('stRes');
const stTime = document.getElementById('stTime');

function setStatus(text, kind) {
  statusEl.textContent = text;
  statusEl.className = 'status' + (kind ? ' ' + kind : '');
}

// ---- Live streaming over the engine-bridge MessagePort --------------------

let enginePort = null;     // MessagePort to the utilityProcess bridge
let streaming = false;
const ctx = canvas.getContext('2d');

// FPS / latency meters.
let fpsCount = 0, fpsLast = performance.now();

// Receive the transferred port relayed by preload via window.postMessage.
window.addEventListener('message', (e) => {
  if (e.data && e.data.type === 'engine-port' && e.ports && e.ports[0]) {
    enginePort = e.ports[0];
    enginePort.onmessage = onEngineMessage;
    enginePort.start();
  }
});

async function onEngineMessage(e) {
  const m = e.data || {};
  if (m.type === 'frame') {
    // Decode the transferred JPEG ArrayBuffer and paint it.
    const bmp = await createImageBitmap(new Blob([m.data], { type: 'image/jpeg' }));
    if (canvas.width !== m.width || canvas.height !== m.height) {
      canvas.width = m.width;
      canvas.height = m.height;
      stRes.textContent = `${m.width} × ${m.height}`;
    }
    ctx.drawImage(bmp, 0, 0);
    bmp.close();

    fpsCount++;
    const now = performance.now();
    if (now - fpsLast >= 1000) {
      stTime.textContent = `${fpsCount} fps (JPEG)`;
      fpsCount = 0;
      fpsLast = now;
    }
  } else if (m.type === 'stream-started') {
    setStatus('串流中…', 'ok');
  } else if (m.type === 'stream-stopped') {
    setStatus('已停止', '');
  } else if (m.type === 'stream-error' || m.type === 'engine-error') {
    setStatus('串流错误: ' + (m.message || ''), 'err');
  }
}

btnStream.addEventListener('click', () => {
  if (!enginePort) { setStatus('引擎桥未就绪', 'err'); return; }
  streaming = !streaming;
  if (streaming) {
    enginePort.postMessage({ type: 'start' });
    btnStream.textContent = '⏹ 停止串流';
    placeholder.hidden = true;
    img.hidden = true;
    canvas.hidden = false;
    stats.hidden = false;
    setStatus('连接引擎…', 'busy');
  } else {
    enginePort.postMessage({ type: 'stop' });
    btnStream.textContent = '▶ 开始串流';
  }
});

// Region screenshot (WeChat-style): launches the fullscreen selection overlay.
btnShot.addEventListener('click', async () => {
  btnShot.disabled = true;
  setStatus('截图中… 拖动选择区域', 'busy');
  try {
    const res = await window.streamApi.startRegionScreenshot();
    if (!res.ok) setStatus('失败: ' + res.error, 'err');
  } catch (e) {
    setStatus('异常: ' + e.message, 'err');
  } finally {
    btnShot.disabled = false;
  }
});

// Notified by main once a region has been copied to the clipboard.
window.streamApi.onScreenshotSaved((data) => {
  stats.hidden = false;
  stRes.textContent = `${data.width} × ${data.height}`;
  stTime.textContent = '已复制到剪贴板';
  setStatus('已复制到剪贴板 ✓', 'ok');
});
window.streamApi.onScreenshotError((data) => {
  setStatus('截图失败: ' + (data.message || ''), 'err');
});
