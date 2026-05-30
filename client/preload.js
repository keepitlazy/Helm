// Bridge between the sandboxed renderer and the main process.
// Only a narrow, explicit API surface is exposed.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('streamApi', {
  captureScreenshot: () => ipcRenderer.invoke('capture:screenshot'),

  // WeChat-style region screenshot: opens the fullscreen selection overlay.
  startRegionScreenshot: () => ipcRenderer.invoke('screenshot:start'),
  onScreenshotSaved: (cb) =>
    ipcRenderer.on('screenshot:saved', (_e, data) => cb(data)),
  onScreenshotError: (cb) =>
    ipcRenderer.on('screenshot:error', (_e, data) => cb(data)),
});

// A MessagePort can't be passed through contextBridge directly. The documented
// pattern: receive the transferred port from main here, then re-post it into the
// page's main world via window.postMessage (which can transfer MessagePorts).
ipcRenderer.on('engine-port', (event) => {
  window.postMessage({ type: 'engine-port' }, '*', event.ports);
});
