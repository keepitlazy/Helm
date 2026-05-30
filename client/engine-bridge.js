// Engine bridge — runs as an Electron utilityProcess (Node env, no DOM).
//
// Topology:
//   renderer ◄─MessagePort─► (this) ◄─named pipe─► stream_sender.exe --serve-pipe
//
// Responsibilities:
//   * own the native engine's lifecycle (spawn / kill)
//   * on the renderer's "start"/"stop", connect/disconnect the named pipe
//   * parse the proto::FrameHeader framing and forward each JPEG to the renderer
//     as a zero-copy transferred ArrayBuffer
//
// MessagePort talks to the renderer; the named pipe talks to the C++ engine —
// exactly the two-boundary split (Mojo inside Electron, OS pipe to the native exe).

const { spawn } = require('child_process');
const net = require('net');

let rendererPort = null;   // MessagePortMain to the renderer
let engineProc = null;     // stream_sender.exe child process
let pipeSock = null;       // net.Socket connected to the engine's named pipe
let senderExe = null;
let pipeName = '\\\\.\\pipe\\stream_demo';

const MAGIC0 = 0x53, MAGIC1 = 0x44, MAGIC2 = 0x4D, MAGIC3 = 0x31; // 'S','D','M','1'

process.parentPort.on('message', (e) => {
  const msg = e.data || {};
  console.error('[bridge] msg ' + (msg.type || '?') + ' ports=' + (e.ports ? e.ports.length : 0));
  if (msg.type === 'init') {
    senderExe = msg.senderExe;
    if (msg.pipeName) pipeName = msg.pipeName;
    if (e.ports && e.ports[0]) {
      rendererPort = e.ports[0];
      rendererPort.on('message', onRendererMessage);
      rendererPort.start();
    }
    startEngine();
  }
});

// NOTE: Electron's MessagePortMain.postMessage(message, transfer) only accepts
// MessagePortMain objects in the transfer list — it does NOT support
// transferring ArrayBuffers (that throws "Port at index N is not a valid port").
// Payloads (e.g. JPEG ArrayBuffers) are passed by value and structured-cloned.
function toRenderer(type, extra) {
  if (rendererPort) rendererPort.postMessage(Object.assign({ type }, extra));
}

function startEngine() {
  engineProc = spawn(senderExe, ['--serve-pipe', pipeName, '15', '0.7'],
                     { stdio: ['pipe', 'pipe', 'pipe'] });

  engineProc.stdout.on('data', (d) => {
    console.error('[engine.out] ' + d.toString().trim());
    if (d.toString().includes('SERVE_PIPE_READY')) toRenderer('engine-ready', {});
  });
  engineProc.stderr.on('data', (d) => console.error('[engine.err] ' + d.toString().trim()));
  engineProc.on('exit', (code) => { console.error('[engine] exit ' + code); toRenderer('engine-exit', { code }); });
  engineProc.on('error', (err) => { console.error('[engine] error ' + err.message); toRenderer('engine-error', { message: err.message }); });
}

function onRendererMessage(e) {
  const msg = e.data || {};
  if (msg.type === 'start') connectPipe();
  else if (msg.type === 'stop') disconnectPipe();
}

function connectPipe() {
  if (pipeSock) return;
  pipeSock = net.connect({ path: pipeName });
  let buf = Buffer.alloc(0);

  pipeSock.on('connect', () => { toRenderer('stream-started', {}); });

  pipeSock.on('data', (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    while (buf.length >= 24) {
      if (!(buf[0] === MAGIC0 && buf[1] === MAGIC1 && buf[2] === MAGIC2 && buf[3] === MAGIC3)) {
        toRenderer('stream-error', { message: 'frame desync (bad magic)' });
        return disconnectPipe();
      }
      const width = buf.readUInt16LE(8);
      const height = buf.readUInt16LE(10);
      const size = buf.readUInt32LE(20);
      if (buf.length < 24 + size) break;            // wait for the rest of the payload

      // Copy the payload into its own ArrayBuffer and hand it to the renderer.
      // (MessagePortMain can't transfer ArrayBuffers, so this is structured-cloned.)
      const ab = buf.buffer.slice(buf.byteOffset + 24, buf.byteOffset + 24 + size);
      buf = buf.subarray(24 + size);
      toRenderer('frame', { width, height, data: ab });
    }
  });

  pipeSock.on('error', (err) => toRenderer('stream-error', { message: err.message }));
  pipeSock.on('close', () => { pipeSock = null; toRenderer('stream-stopped', {}); });
}

function disconnectPipe() {
  if (pipeSock) { pipeSock.destroy(); pipeSock = null; }
}

function shutdown() {
  disconnectPipe();
  if (engineProc) { try { engineProc.kill(); } catch (_) {} engineProc = null; }
}

process.on('exit', shutdown);
process.parentPort.on('close', shutdown);
