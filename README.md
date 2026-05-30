# Helm — Low-Latency Screen Streaming & Remote Desktop Control

A Windows screen-streaming engine with an Electron client. It started as a
low-latency, single-machine streaming demo (GPU capture → hardware encode →
self-rolled framing → Electron + WebCodecs) and is evolving toward **two-machine,
bidirectional remote desktop control** over the public internet via WebRTC.

> Status: the streaming/capture pipeline and the Electron viewer work today.
> The remote-control direction (input injection, WebRTC transport, signaling)
> is the next milestone — see [docs/remote_desktop_architecture.html](docs/remote_desktop_architecture.html).

## Architecture

Two design documents describe where the project is and where it's going:

- **[docs/remote_desktop_architecture.html](docs/remote_desktop_architecture.html)** —
  the target remote-desktop architecture: Controller (Electron) ⇄ Signaling/TURN
  ⇄ Host Agent (Electron + native input injector), video over WebRTC media track,
  keyboard/mouse over an `RTCDataChannel`, injected on the host via native `SendInput`.
- The original single-machine streaming diagram (`streaming_architecture.html`)
  documents the current capture → encode → transport → decode → render pipeline.

The video path is intended to run on Chromium's built-in WebRTC (congestion
control, NACK, bandwidth estimation come for free). The existing native
`WGC → NV12 → H.264` pipeline is retained as an optional low-latency LAN bypass.

## Repository layout

```
sender/    Native C++ capture/encode engine (Windows)
  src/capture/    Windows Graphics Capture (WGC) → ID3D11Texture2D
  src/process/    D3D11 compute shader BGRA → NV12
  src/encode/     Media Foundation (QSV/MF) H.264; optional NVENC backend
  src/input/      InputInjector — SendInput (to be completed for remote control)
  src/transport/  Self-rolled framing over TCP / named pipe (proto::FrameHeader)
client/    Electron client (viewer / control plane)
  main.js           Electron main process
  engine-bridge.js  utilityProcess bridge: named pipe ⇄ renderer MessagePort
  renderer/         UI + WebCodecs decode / render
docs/      Architecture documents
```

## Prerequisites

- Windows 10/11 with a GPU supporting Windows Graphics Capture
- Visual Studio 2022 (MSVC) + CMake ≥ 3.20
- Node.js 18+ and npm
- Optional: NVIDIA Video Codec SDK to enable the NVENC backend

## Build the native sender

```powershell
cd sender
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# Optional NVENC backend:
# cmake -B build -A x64 -DNVENC_SDK_DIR="C:/path/to/Video_Codec_SDK"
```

Output: `sender/build/Release/stream_sender.exe`.

The sender runs standalone in several modes:

```powershell
stream_sender.exe --screenshot out.bmp     # capture one frame to BMP, exit
stream_sender.exe --demo 5                  # 5s capture FPS test
stream_sender.exe --serve 8787 15 0.7       # framed JPEG over TCP (port fps quality)
stream_sender.exe --serve-pipe <name> 15 0.7 # framed JPEG over a named pipe
```

## Run the Electron client

```powershell
cd client
npm install
npm start
```

The client spawns the native sender, streams frames over a named pipe, and
renders them. Package a standalone Windows build with `npm run package`
(bundles `stream_sender.exe` as an extra resource).

## License

Not yet specified.
