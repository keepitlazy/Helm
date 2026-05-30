// Preload for the fullscreen region-screenshot overlay window.
// Exposes a tiny, explicit API to the overlay renderer.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('overlayApi', {
  // main → overlay: the captured desktop image to select from.
  onImage: (cb) => ipcRenderer.on('overlay:image', (_e, data) => cb(data)),

  // overlay → main: the new frame has painted; safe to reveal the window.
  shown: () => ipcRenderer.send('overlay:shown'),

  // overlay → main: the cropped region as PNG bytes (ArrayBuffer), plus size.
  commit: (png, width, height) =>
    ipcRenderer.send('overlay:commit', { png, width, height }),

  // overlay → main: user cancelled (Esc / right-click / blur).
  cancel: () => ipcRenderer.send('overlay:cancel'),
});
