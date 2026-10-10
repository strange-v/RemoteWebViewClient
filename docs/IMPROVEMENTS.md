# ESP32 client: correctness and performance improvements

Review of `components/remote_webview`, started from `hw_test` (`e89c3e4`). Work is on branch `improvements`.
Measurements were taken on an ESP32-S3 (480×480 `st7701s` RGB panel, `gt911` touch, software JPEG).
Items marked **(verify)** depend on behavior that still has to be confirmed on a device.

## Status

| ID | Area | Status | Commit |
|----|------|--------|--------|
| C1 | Bounds checks overflow on 32-bit | ✅ Done | `6627ae2` |
| C2 | WebSocket reconnect logic | ✅ Done | `6627ae2` |
| C3 | Dropped packets leave stale screen regions | ✅ Done (full-frame request + bounded backpressure) | `6627ae2`, `de0b456` |
| C4 | Clipped JPEG blocks drawn with wrong stride | ✅ Done | `6627ae2` |
| C5 | Duplicate / malformed touch events | ✅ Done | `6627ae2` |
| C6 | Data races between tasks | ✅ Done | `6627ae2` |
| C7 | Unchecked resource creation, init order | ✅ Done | `6627ae2` |
| C8 | Decode failures ignored | ✅ Done | `6627ae2` |
| C9 | Display accessed from a non-main task | 🟡 Documented; validation open | `bc2a75f` |
| C10 | Minor issues | 🟡 Partly done | `1d54780`, `eeb473a` |
| C11 | Watchdog reset under a continuous stream of frames | ✅ Done | `de0b456` |
| P1 | `-O2` instead of `-Os` | ✅ Measured: ~4%, not worth recommending | `bba7909` |
| P2 | Wi-Fi power save, lwIP tuning | 🟡 `power_save_mode: none` measured and documented; lwIP open | — |
| P3 | Logging in hot paths | ✅ Done | `eeb473a`, `de0b456` |
| P4 | Instrumentation | ✅ Done (`stats_interval`) | `fe5becb`, `5520c71` |
| P5 | Receive buffer pool | ⬜ Open, low priority | — |
| P6 | P4 HW decoder input buffer size | ⬜ Open | — |
| P9 | Non-blocking touch sends | ⬜ Open, low priority | — |
| P10 | Memory footprint | ⬜ Open, low priority | — |

Dropped after measuring: **P7** (fewer copies / pipelining on the P4 hardware path) and **P8** (decode from
internal RAM). The client is ~20% of the touch-to-content latency, decoding is bound by per-pixel work in
JPEGDEC's SIMD assembly, and drawing to the RGB framebuffer is cheap. Neither would change what the user sees.

---

## Baseline measurements

`stats_interval: 10s`, `-O2`, before the C11 change.

| Scenario | Metric | Value |
|---|---|---|
| Full frame 480×480 | decode / draw | ~35 / ~8 ms |
| Full frame | decode vs. JPEG size | 1.6 KB → 23 ms, 23 KB → 45 ms (pixel-bound) |
| Partial frame | total | 6–12 ms |
| Tab switch | touch → first frame | ~35 ms |
| Tab switch | touch → page content | ~230 ms (max 365) |
| All | client CPU busy at normal use | ~3% |
| All | dropped / queue max | 0 / ≤ 2 of 12 |

Takeaway: in normal use the client is not the bottleneck. Most of the tab-switch latency is the browser
rendering the new view plus the server pipeline.

---

## Done

**C1. Bounds checks.** Length checks compare against the remaining length (`th.dlen > len - off`,
`url_len > len - header`) instead of adding, so they can't wrap. Tiles whose origin is outside the display are
rejected. The decoded JPEG size is not compared with the tile header; C4 clipping makes that harmless.

**C2. Reconnect.** No `esp_websocket_client_stop()` from the event handler; reconnects are left to the client's
auto-reconnect. `rwv_ws` restarts the client only after 60 s without a connection. `ESP_ERROR_CHECK` on client
init was replaced with logged retries, so low memory no longer causes a boot loop.

**C3 + C8. Lost or failed tiles.** Every message that doesn't reach the decoder and every tile that fails to
decode marks the screen out of sync. `rwv_ws` then sends `RequestFullFrame` (message type 7, at most every 2 s)
until a full frame arrives and decodes without any loss. The server re-processes its last screencast frame, so
this also works on a static page. `on_frame_update` fires only for frames without failed tiles.

**C4. Clipping.** One helper clips to the tile and display bounds and passes `x_offset`/`y_offset`/`x_pad` to
`draw_pixels_at`, so row stride stays correct. Used by both the software and the P4 hardware path.

**C5. Touch.** The server handles a single pointer, so only the first finger is forwarded. Each Down/Move/Up is
sent once; ESPHome states are tested as bit flags; Up carries the last coordinates; a throttled move is flushed
before Up; a second finger doesn't start a new gesture until all fingers are lifted.

**C6. Data races.** `url_` is guarded by `state_mtx_`; `ws_client_` and `last_keepalive_us_` are atomics; all
sends go through `ws_send_bin_()`, which loads the client handle once.

**C7. Init.** Queue, mutex and task creation are checked (`mark_failed()` on failure); tasks start after the
hardware decoder is initialized.

**C11. Watchdog under a continuous stream of full frames.** Reproduced with frames sent back to back: the decode
queue stayed full, packets were dropped, and `loopTask` starved until the task watchdog reset the device.
- The WS handler waits up to 1 s for space in the decode queue before dropping. While it waits the socket isn't
  read, so TCP flow control lets the server's backpressure skip stale frames. The handler runs without the
  client lock (checked in `esp_websocket_client` 1.5.0), so touch sends and pings are not blocked.
- `rwv_decode` runs at priority 1, the same as ESPHome's `loopTask` on CPU 1. FreeRTOS time-slices equal
  priorities every tick, so touch input is handled within ~1 ms even during a 45 ms full-frame decode, while
  the mostly idle `loopTask` costs decoding almost nothing. A higher priority delays touch input by a whole
  frame decode; CPU 0 would compete with Wi-Fi/lwIP and starve `IDLE0`, which the watchdog checks.
  `IDLE1` is not checked (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`).
- `FrameStats` still goes through the decode queue, so its reply still means "everything before it is drawn".

**P1. `-O2`.** Measured with `CONFIG_COMPILER_OPTIMIZATION_PERF=y`: full-frame decode ~36 → ~34.5 ms (~4%).
The heavy parts of the JPEGDEC fork are SIMD assembly, which compiler flags don't affect. The README line
`COMPILER_OPTIMIZATION_SIZE: y` had no effect (missing `CONFIG_` prefix) and was removed.

**P3. Logging.** Per-frame log is VERBOSE; HW fallback reasons are VERBOSE with a summary warning at most every
10 s; "decode queue full" is logged at most every 5 s with a count.

**P4. Instrumentation.** `stats_interval` logs, per window: fps, KiB/s, per-frame rx / queue / decode / draw /
total time, queue high-water mark, drops, failed tiles, HW fallbacks, full-frame requests, and touch-to-frame
latency (first frame and largest frame within 2 s of a touch Up).

---

## Open

### C9. Display ownership — validation

README states that nothing else may draw to the display (`bc2a75f`). Still open:
- **Compile-time check** in `__init__.py` (`FINAL_VALIDATE_SCHEMA`): error if the display has `lambda`/`pages`
  or `update_interval` isn't `never`; warn on `auto_clear_enabled`/`show_test_card`, and when touch shares the
  display's SPI bus.
- **Optional `remote_webview.pause` / `resume` actions** to draw an overlay safely: pause decoding, let the user
  update the display, then resume with a full-frame request (the mechanism from C3 already exists).

### C10. Minor — remaining

Done: random device-ID fallback (`1d54780`), URL-encoded `device_id`, rate-limited HW fallback warnings
(`eeb473a`). Remaining:
- `REMOTE_WEBVIEW_HAS_CACHE_MSYNC` / `esp_cache.h` are unused (`remote_webview.h`).
- `espressif/esp-dsp` in `idf_component.yml` isn't used by this component; remove it if the JPEGDEC fork
  doesn't need it (the fork has its own SIMD `.S` files).
- `examples/*.yaml` still contain the ineffective `COMPILER_OPTIMIZATION_SIZE` line.
- **(verify, needs a P4 device)** The HW decoder outputs a fixed RGB565 byte order, but `rgb565_big_endian_`
  is passed to `draw_pixels_at` on that path too. Check colors with both `big_endian` settings.

### P2. Wi-Fi and network settings

YAML-only experiments, measure with `stats_interval` (touch → content):
- ✅ `wifi: power_save_mode: none` — measured on ESP32-S3: default power saving added ~30 ms per update,
  cut throughput from ~1.6 MB/s to ~330 KB/s, and a full-screen change took 154 ms instead of 88 ms.
  Recommended in README.
- `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0: y` — keeps lwIP off CPU 1, which is left to `loopTask` and decoding.
- Optionally `CONFIG_LWIP_TCP_WND_DEFAULT`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE`,
  `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` — only if rx time shows up.

### P5. Receive buffer pool (low priority)

Every WS message is `heap_caps_malloc`/`free`d in PSRAM. A pool of N buffers of `max_bytes_per_msg` would remove
allocator cost and fragmentation and would be a natural backpressure point. Measurements show no allocator
bottleneck so far; do it only if fragmentation or `malloc failed` shows up in long runs.

### P6. P4 hardware decoder input buffer

`setup()` sizes the compressed **input** buffer like a decompressed RGB565 frame (480×480 → 450 KiB) while the
largest message is ~60 KiB. Size it from `max_bytes_per_msg` with the required DMA alignment. Saves ~390 KiB of
PSRAM on ESP32-P4.

### P9. Non-blocking touch sends (low priority)

Touch events are sent from ESPHome's main loop and can block it for up to ~60 ms (10 ms send mutex + 50 ms send
timeout) if another send holds the mutex or the socket is slow. A small queue drained by `rwv_ws` would keep the
loop non-blocking. Not observed as a problem so far.

### P10. Memory footprint (low priority)

- `rwv_decode` has a 32 KiB stack although `JPEGDEC` is a class member; check `uxTaskGetStackHighWaterMark()`
  and shrink to free internal RAM.
- **(verify)** where `esp_websocket_client` allocates its 30 KiB RX/TX buffers (internal vs PSRAM).
