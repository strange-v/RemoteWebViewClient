# Remote WebView Client

Here you can find test clients for [Remote WebView Server](https://github.com/strange-v/RemoteWebViewServer)

## Guition-ESP32-S3-4848S040
Implements the simplest (and likely fastest) rendering path: JPEG tiles are decoded and written directly into the panel’s line buffer.

### Trade-offs

- Slight tearing during fast scrolls or big updates.
- Brief blur/compression artifacts on fine text (JPEG).
- Occasional stale patches until the next refresh/full-frame.
