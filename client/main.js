// Electron main process.
//
// Today: drives the native WGC capture engine (stream_sender.exe --screenshot)
// as a child process and hands the captured frame to the UI. This is the
// control plane that will later be extended to start/stop a live stream and
// receive encoded frames over TCP.

const { app, BrowserWindow, ipcMain, utilityProcess, MessageChannelMain,
        screen, clipboard, nativeImage } = require('electron');
const { execFile } = require('child_process');
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
  win.on('closed', () => { if (mainWindow === win) mainWindow = null; });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));

  // Spawn the engine bridge (utilityProcess) and hand one end of a MessageChannel
  // to the renderer so it talks to the bridge directly, off the main thread.
  win.webContents.once('did-finish-load', () => startEngineBridge(win.webContents));
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
function captureScreenshot() {
  if (!fs.existsSync(SENDER_EXE)) {
    return Promise.resolve({ ok: false, error: `sender not built: ${SENDER_EXE}` });
  }
  const outPath = path.join(os.tmpdir(), `stream_demo_shot_${Date.now()}.bmp`);
  const started = Date.now();

  return new Promise((resolve) => {
    execFile(SENDER_EXE, ['--screenshot', outPath], { timeout: 12000 },
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

ipcMain.handle('capture:screenshot', () => captureScreenshot());

// ---- WeChat-style region screenshot ---------------------------------------
//
// Flow: native WGC grabs the whole desktop → a frameless fullscreen overlay
// window shows that image → user drags a rectangle → on confirm the renderer
// crops the region to a PNG and ships the bytes back here → we drop it on the
// system clipboard. Esc / right-click / blur cancels.

let overlayWin = null;

function closeOverlay() {
  if (overlayWin) { try { overlayWin.close(); } catch (_) {} overlayWin = null; }
}

async function startRegionScreenshot() {
  if (overlayWin) return { ok: false, error: 'overlay already open' };

  // Hide our control window first so it isn't captured, then give the desktop
  // compositor a moment to repaint before the native engine grabs a frame.
  if (mainWindow) mainWindow.hide();
  await new Promise((r) => setTimeout(r, 180));

  const shot = await captureScreenshot();
  if (!shot.ok) {
    if (mainWindow) mainWindow.show();
    return { ok: false, error: shot.error };
  }

  // Cover the display the cursor is on (full bounds, including taskbar).
  const cursor = screen.getCursorScreenPoint();
  const disp = screen.getDisplayNearestPoint(cursor);
  const { x, y, width, height } = disp.bounds;

  overlayWin = new BrowserWindow({
    x, y, width, height,
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
    backgroundColor: '#000000',
    webPreferences: {
      preload: path.join(__dirname, 'preload-overlay.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  overlayWin.setAlwaysOnTop(true, 'screen-saver');
  overlayWin.removeMenu();

  const win = overlayWin;
  win.on('closed', () => {
    if (overlayWin === win) overlayWin = null;
    if (mainWindow) mainWindow.show();
  });

  await win.loadFile(path.join(__dirname, 'renderer', 'overlay.html'));
  win.webContents.send('overlay:image', {
    url: shot.url, width: shot.width, height: shot.height,
  });
  win.focus();
  return { ok: true };
}

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
  closeOverlay();
});

ipcMain.on('overlay:cancel', () => closeOverlay());

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

app.on('will-quit', () => { if (engineBridge) { try { engineBridge.kill(); } catch (_) {} } });

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});
