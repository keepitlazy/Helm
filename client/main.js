// Electron main process.
//
// Today: drives the native WGC capture engine (stream_sender.exe --screenshot)
// as a child process and hands the captured frame to the UI. This is the
// control plane that will later be extended to start/stop a live stream and
// receive encoded frames over TCP.

const { app, BrowserWindow, ipcMain, utilityProcess, MessageChannelMain,
        screen, clipboard, nativeImage } = require('electron');
const { execFile, spawn } = require('child_process');
const path = require('path');
const os = require('os');
const fs = require('fs');
const { pathToFileURL } = require('url');

const PIPE_NAME = '\\\\.\\pipe\\stream_demo';

// This box's C: drive is full, so keep Electron's cache/temp on the app's drive
// (Chromium's disk cache + our screenshot files would otherwise fail with ENOSPC).
const DATA_DIR = path.join(path.parse(__dirname).root, 'stream-demo-data');
try { fs.mkdirSync(DATA_DIR, { recursive: true }); } catch (_) {}
app.setPath('userData', path.join(DATA_DIR, 'userData'));
app.setPath('temp', DATA_DIR);
process.env.TMP = process.env.TEMP = DATA_DIR;

// Path to the native sender. In a packaged build it's bundled as an extra
// resource next to the app; in dev it's the CMake Release output.
const SENDER_EXE = app.isPackaged
  ? path.join(process.resourcesPath, 'stream_sender.exe')
  : path.join(__dirname, '..', 'sender', 'build', 'Release', 'stream_sender.exe');

let mainWindow = null;

function createWindow() {
  const win = new BrowserWindow({
    width: 1180,
    height: 820,
    backgroundColor: '#0a0e1a',
    title: 'Screen Streaming Demo',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  win.removeMenu();
  mainWindow = win;
  win.on('closed', () => {
    if (mainWindow === win) mainWindow = null;
    // Tear down the pre-warmed overlay too; a lingering hidden window would
    // otherwise keep the app alive after the main window closes.
    if (overlayWin) { try { overlayWin.destroy(); } catch (_) {} overlayWin = null; }
  });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));

  // Spawn the engine bridge (utilityProcess) and hand one end of a MessageChannel
  // to the renderer so it talks to the bridge directly, off the main thread.
  win.webContents.once('did-finish-load', () => {
    startEngineBridge(win.webContents);
    warmOverlay();          // pre-create + load the screenshot overlay so it shows instantly later
    startSnapshotService(); // keep a warm WGC capture running so screenshots have no spawn latency
  });
}

let engineBridge = null;

function startEngineBridge(targetWebContents) {
  if (engineBridge) return;
  const { port1, port2 } = new MessageChannelMain();
  engineBridge = utilityProcess.fork(path.join(__dirname, 'engine-bridge.js'));
  engineBridge.once('spawn', () => {
    engineBridge.postMessage({ type: 'init', senderExe: SENDER_EXE, pipeName: PIPE_NAME }, [port1]);
  });
  engineBridge.once('exit', () => { engineBridge = null; });
  targetWebContents.postMessage('engine-port', null, [port2]);
}

// Capture one desktop frame via the native engine, return a viewable URL.
// `physPoint` (optional {x,y} in physical screen pixels) selects the monitor
// under that point — per-display capture; omit it to capture the primary.
function captureScreenshot(physPoint) {
  if (!fs.existsSync(SENDER_EXE)) {
    return Promise.resolve({ ok: false, error: `sender not built: ${SENDER_EXE}` });
  }
  const outPath = path.join(os.tmpdir(), `stream_demo_shot_${Date.now()}.bmp`);
  const started = Date.now();

  const args = ['--screenshot', outPath];
  if (physPoint) args.push(String(Math.round(physPoint.x)), String(Math.round(physPoint.y)));

  return new Promise((resolve) => {
    execFile(SENDER_EXE, args, { timeout: 12000 },
      (err, stdout) => {
        const elapsedMs = Date.now() - started;
        const okLine = /SCREENSHOT_OK\s+\S+\s+(\d+)x(\d+)/.exec(stdout || '');
        if (okLine && fs.existsSync(outPath)) {
          resolve({
            ok: true,
            url: pathToFileURL(outPath).href,
            width: Number(okLine[1]),
            height: Number(okLine[2]),
            elapsedMs,
          });
        } else {
          resolve({
            ok: false,
            error: (err && err.message) || (stdout || '').trim() || 'capture failed',
            elapsedMs,
          });
        }
      });
  });
}

// ---- Resident snapshot service ---------------------------------------------
//
// `stream_sender.exe --snapshot-serve` keeps a live WGC capture warm per monitor
// and serves screenshots from the newest frame on demand. This removes the
// process-spawn + capture-warm-up latency (~100-300ms) from every screenshot, so
// the overlay freeze appears effectively instantly. We talk to it over stdin/
// stdout: write "snap\t<x>\t<y>\t<path>\n", read back "SNAPSHOT_OK <path> WxH".
let snapProc = null;
let snapReady = false;
const snapQueue = [];           // FIFO of pending { resolve, outPath } awaiting a reply
let snapStdoutBuf = '';

function startSnapshotService() {
  if (snapProc || !fs.existsSync(SENDER_EXE)) return;
  snapProc = spawn(SENDER_EXE, ['--snapshot-serve'], { stdio: ['pipe', 'pipe', 'pipe'] });

  snapProc.stdout.on('data', (chunk) => {
    snapStdoutBuf += chunk.toString('utf8');
    let nl;
    while ((nl = snapStdoutBuf.indexOf('\n')) >= 0) {
      const line = snapStdoutBuf.slice(0, nl).trim();
      snapStdoutBuf = snapStdoutBuf.slice(nl + 1);
      if (!line) continue;
      if (line === 'SNAPSHOT_READY') { snapReady = true; continue; }
      const ok = /^SNAPSHOT_OK\s+\S+\s+(\d+)x(\d+)/.exec(line);
      if (ok) {
        const job = snapQueue.shift();
        if (job) job.resolve({
          ok: true,
          url: pathToFileURL(job.outPath).href,
          width: Number(ok[1]),
          height: Number(ok[2]),
          elapsedMs: Date.now() - job.started,
        });
      } else if (/^SNAPSHOT_FAIL/.test(line)) {
        const job = snapQueue.shift();
        if (job) job.resolve({ ok: false, error: line });
      }
    }
  });

  // If the engine dies, reject anything in flight and let the next request fall
  // back to a one-shot capture (and respawn the service).
  const onGone = () => {
    snapReady = false;
    snapProc = null;
    while (snapQueue.length) snapQueue.shift().resolve({ ok: false, error: 'snapshot engine exited' });
  };
  snapProc.once('exit', onGone);
  snapProc.once('error', onGone);
}

// Serve a screenshot from the warm engine; fall back to a one-shot capture if the
// engine isn't ready. `physPoint` selects the monitor (physical pixels); omit/null
// -> primary (encoded as x=-1 on the wire).
function snapshot(physPoint) {
  if (!snapProc || !snapReady) {
    startSnapshotService();              // (re)start for next time
    return captureScreenshot(physPoint); // this time, fall back to one-shot
  }
  const outPath = path.join(os.tmpdir(), `stream_demo_shot_${Date.now()}.bmp`);
  const x = physPoint ? Math.round(physPoint.x) : -1;
  const y = physPoint ? Math.round(physPoint.y) : 0;
  return new Promise((resolve) => {
    snapQueue.push({ resolve, outPath, started: Date.now() });
    try {
      snapProc.stdin.write(`snap\t${x}\t${y}\t${outPath}\n`);
    } catch (err) {
      snapQueue.pop();
      resolve(captureScreenshot(physPoint));
    }
  });
}

ipcMain.handle('capture:screenshot', () => snapshot());

// ---- WeChat-style region screenshot ---------------------------------------
//
// Flow: native WGC grabs the whole desktop → a frameless fullscreen overlay
// window shows that image → user drags a rectangle → on confirm the renderer
// crops the region to a PNG and ships the bytes back here → we drop it on the
// system clipboard. Esc / right-click / blur cancels.

let overlayWin = null;          // persistent, pre-warmed fullscreen overlay window
let overlayReadyPromise = null; // resolves when overlay.html has finished loading
let overlayActive = false;      // a selection session is currently on screen
let overlayBounds = null;       // target monitor bounds (DIP) for the current session

// Pre-create the overlay window once and load its HTML, kept hidden and reused
// across screenshots. This removes the window-creation + page-load latency from
// the capture path, so triggering a screenshot only costs "send image + show".
function warmOverlay() {
  if (overlayWin) return;
  const win = new BrowserWindow({
    show: false,
    frame: false,
    transparent: false,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    skipTaskbar: true,
    alwaysOnTop: true,
    hasShadow: false,
    fullscreenable: false,
    thickFrame: false,                // drop WS_THICKFRAME -> no resize border on Windows
    roundedCorners: false,            // Win11 draws rounded corners on frameless windows;
                                      // they expose the desktop at the 4 corners (looks like a border)
    enableLargerThanScreen: true,     // allow covering the full monitor bounds exactly
    backgroundColor: '#000000',
    paintWhenInitiallyHidden: true,   // render while hidden so the first show is instant
    webPreferences: {
      preload: path.join(__dirname, 'preload-overlay.js'),
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,    // keep painting while hidden
    },
  });
  win.setAlwaysOnTop(true, 'screen-saver');
  win.removeMenu();
  win.on('closed', () => {
    if (overlayWin === win) { overlayWin = null; overlayReadyPromise = null; overlayActive = false; }
  });
  overlayReadyPromise = win.loadFile(path.join(__dirname, 'renderer', 'overlay.html'));
  overlayWin = win;
}

// Hide (not destroy) the overlay after a session so it stays warm for next time.
function hideOverlay() {
  overlayActive = false;
  if (overlayWin) { try { overlayWin.hide(); } catch (_) {} }
}

async function startRegionScreenshot() {
  if (overlayActive) return { ok: false, error: 'overlay already open' };

  // The overlay window is pre-warmed at startup; make sure it exists & is loaded.
  if (!overlayWin) warmOverlay();
  await overlayReadyPromise;

  // Per-display capture: target the monitor under the cursor, captured at its
  // native (physical) resolution. dipToScreenPoint converts the DIP cursor to
  // the physical-pixel space the native MonitorFromPoint expects.
  const cursorDip = screen.getCursorScreenPoint();
  const disp = screen.getDisplayNearestPoint(cursorDip);
  const cursorPhys = screen.dipToScreenPoint(cursorDip);

  // WeChat-style frozen frame: capture whatever is on screen *without* hiding our
  // own window first. No hide/repaint wait means no visible seam before the freeze.
  // Served from the resident warm engine, so there's no spawn/warm-up latency.
  const shot = await snapshot(cursorPhys);
  if (!shot.ok) return { ok: false, error: shot.error };
  if (!overlayWin) return { ok: false, error: 'overlay gone' };

  // Cover that same display 1:1 (its full bounds, including taskbar). The image's
  // physical px == the display's physical px, and the window covers the display
  // in DIP, so devicePixelRatio maps each image pixel to one device pixel.
  overlayBounds = disp.bounds;
  overlayWin.setBounds(overlayBounds);

  overlayActive = true;
  // Hand the frozen image to the already-loaded overlay. It paints the image and
  // calls back 'overlay:shown'; only then do we reveal the window, so the user
  // never sees a blank or stale frame — the freeze appears in a single paint.
  overlayWin.webContents.send('overlay:image', {
    url: shot.url, width: shot.width, height: shot.height,
  });
  return { ok: true };
}

// Overlay reports the new frame is painted → reveal the window in one shot.
ipcMain.on('overlay:shown', () => {
  if (!overlayActive || !overlayWin) return;
  overlayWin.setAlwaysOnTop(true, 'screen-saver');
  // showInactive + re-assert bounds: setBounds on a still-hidden window can be
  // clamped/ignored on Windows, so apply it again now that the window is mapped
  // to guarantee it covers the whole monitor (no border, truly fullscreen).
  overlayWin.show();
  if (overlayBounds) overlayWin.setBounds(overlayBounds);
  overlayWin.focus();
});

ipcMain.handle('screenshot:start', () => startRegionScreenshot());

ipcMain.on('overlay:commit', (_e, payload) => {
  // payload.png is an ArrayBuffer of PNG bytes for the cropped region.
  try {
    const img = nativeImage.createFromBuffer(Buffer.from(payload.png));
    clipboard.writeImage(img);
    if (mainWindow) {
      mainWindow.webContents.send('screenshot:saved', {
        width: payload.width, height: payload.height,
      });
    }
  } catch (err) {
    if (mainWindow) {
      mainWindow.webContents.send('screenshot:error', { message: err.message });
    }
  }
  hideOverlay();
});

ipcMain.on('overlay:cancel', () => hideOverlay());

// Headless end-to-end check: native capture + Chromium BMP decode, then exit.
async function runSelfTest() {
  const res = await captureScreenshot();
  console.log('SELFTEST_RESULT ' + JSON.stringify(res));
  if (!res.ok) { app.exit(1); return; }

  const win = new BrowserWindow({ show: false, webPreferences: { contextIsolation: true } });
  await win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  const decoded = await win.webContents.executeJavaScript(
    `new Promise((r) => {
       const i = new Image();
       i.onload = () => r(i.naturalWidth + 'x' + i.naturalHeight);
       i.onerror = () => r('DECODE_ERROR');
       i.src = ${JSON.stringify(res.url)};
     })`
  ).catch((e) => 'EXC:' + e.message);
  console.log('SELFTEST_IMG ' + decoded);
  const good = /^\d+x\d+$/.test(String(decoded));
  app.exit(good ? 0 : 2);
}

// Headless end-to-end streaming check: utilityProcess bridge + named pipe +
// native engine + MessagePort frame transfer + Chromium JPEG decode, then exit.
async function runStreamSelfTest() {
  const { port1, port2 } = new MessageChannelMain();
  const bridge = utilityProcess.fork(path.join(__dirname, 'engine-bridge.js'));
  await new Promise((resolve) => bridge.once('spawn', resolve));
  bridge.postMessage({ type: 'init', senderExe: SENDER_EXE, pipeName: PIPE_NAME }, [port1]);

  let frames = 0;
  let firstInfo = null;
  let jpegPath = null;

  port2.on('message', (e) => {
    const m = e.data || {};
    if (m.type !== 'frame') console.log('SELFTEST_MSG ' + JSON.stringify(m));
    if (m.type === 'engine-ready') {
      port2.postMessage({ type: 'start' });
    } else if (m.type === 'frame') {
      frames++;
      if (!firstInfo) {
        firstInfo = `${m.width}x${m.height} bytes=${m.data.byteLength}`;
        jpegPath = path.join(DATA_DIR, 'selftest_stream.jpg');
        fs.writeFileSync(jpegPath, Buffer.from(m.data));
      }
    }
  });
  port2.start();

  await new Promise((r) => setTimeout(r, 3500));
  port2.postMessage({ type: 'stop' });
  console.log(`SELFTEST_STREAM frames=${frames} first=${firstInfo}`);

  let decoded = 'NO_FRAME';
  if (jpegPath) {
    const win = new BrowserWindow({ show: false, webPreferences: { contextIsolation: true } });
    await win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
    decoded = await win.webContents.executeJavaScript(
      `new Promise((res) => {
         const i = new Image();
         i.onload = () => res(i.naturalWidth + 'x' + i.naturalHeight);
         i.onerror = () => res('DECODE_ERROR');
         i.src = ${JSON.stringify(pathToFileURL(jpegPath).href)};
       })`
    ).catch((e) => 'EXC:' + e.message);
  }
  console.log('SELFTEST_STREAM_IMG ' + decoded);
  try { bridge.kill(); } catch (_) {}
  const good = frames > 0 && /^\d+x\d+$/.test(String(decoded));
  app.exit(good ? 0 : 5);
}

if (process.argv.includes('--selftest')) {
  app.whenReady().then(runSelfTest);
} else if (process.argv.includes('--selftest-stream')) {
  app.whenReady().then(runStreamSelfTest);
} else {
  app.whenReady().then(createWindow);
}

app.on('will-quit', () => {
  if (engineBridge) { try { engineBridge.kill(); } catch (_) {} }
  if (snapProc) { try { snapProc.stdin.write('quit\n'); } catch (_) {} try { snapProc.kill(); } catch (_) {} }
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});
