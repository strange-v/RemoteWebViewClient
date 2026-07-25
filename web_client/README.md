# Browser Test Client

This folder contains a browser-based test client for faster development loops without reflashing ESP devices.

## Current features

- WebSocket connection with reconnect and keepalive.
- Binary protocol compatibility for Frame, Touch, OpenURL, and Keepalive packets.
- JPEG tile rendering to HTML5 canvas.
- Pointer/touch forwarding (down/move/up) with move coalescing.

## Run locally

```bash
npm install
npm run dev
```

Open the local Vite URL in your browser, set server/display params, click Connect, then use Send open_url for navigation tests.
