# ESP32 client: correctness and performance improvements

Review of `components/remote_webview` (branch `hw_test`, commit `e89c3e4`).
Static analysis only: nothing here has been built or measured on hardware.
Items marked **(verify)** depend on library behavior that should be confirmed on a device.

The overall architecture is sound: WebSocket receive and JPEG decode run in separate tasks,
software JPEG output is drawn in blocks, and ESPHome automations are dispatched from `loop()`.
The issues below are about robustness at the edges and about throughput/latency.

## Priority overview

| ID | Area | Severity | Effort |
|----|------|----------|--------|
| C1 | Bounds checks overflow on 32-bit | High (memory safety) | Small |
| C2 | WebSocket reconnect logic fights the client library | High (reliability) | Small |
| C3 | Dropped packets leave stale screen regions | High (visual) | Medium |
| C4 | Clipped JPEG blocks drawn with wrong stride | Medium (visual) | Small |
| C5 | Duplicate / malformed touch events | Medium (UX) | Medium |
| C6 | Data races between tasks | Medium | Small–Medium |
| C7 | Unchecked resource creation, init order | Medium | Small |
| C8 | Decode failures ignored | Low–Medium | Small |
| C9 | Display accessed from a non-main task | Low (config dependent) | — |
| C10 | Minor issues | Low | Small |
| P1–P10 | Performance | — | see below |

Suggested order: C1, C2, C3 + C8, C5, C4, then add instrumentation (P4) and only then the buffer/pipeline work.

---

## Correctness

### C1. Bounds checks can overflow (memory safety)

- `remote_webview.cpp:397` — `if (off + th.dlen > len) return;`
- `remote_webview.cpp:41` — `if (sizeof(proto::CurrentURLHeader) + hdr->url_len > len) return;`

`size_t` is 32-bit on ESP32. A `dlen`/`url_len` close to `0xFFFFFFFF` wraps the sum, the check passes, and
the decoder reads out of bounds (tile) or `std::string` tries to allocate ~4 GB and aborts (URL).

Fix: compare against the remaining length instead of adding.

```cpp
if (off > len || th.dlen > len - off) return;
// ...
const size_t avail = len - sizeof(proto::CurrentURLHeader);
if (hdr->url_len > avail) return;
```

Also validate tile geometry: `th.x + th.w <= display_width_`, `th.y + th.h <= display_height_`
(currently only `w`/`h` alone are checked), and that the decoded JPEG size matches `th.w`/`th.h`.
The `(int16_t)th.x` cast turns coordinates above 32767 into negative values.

### C2. WebSocket reconnect logic

- `remote_webview.cpp:273`, `:282` — `DISCONNECTED`/`CLOSED` handlers call `websocket_force_reconnect()`,
  i.e. `esp_websocket_client_stop()` + `start()`. Espressif documents that `stop()` must not be called from the
  WebSocket event handler (it runs in the client task). The library returns `ESP_FAIL` and logs an error;
  `start()` then fails too because the client is still running. Return values are ignored, so these calls are
  no-ops that only spam the log.
- `remote_webview.cpp:231` — the `rwv_ws` watchdog does stop+start every 5 s while disconnected, while
  auto-reconnect is also enabled (`reconnect_timeout_ms = 2000`, `network_timeout_ms = 10000`). A connect
  attempt that takes longer than ~5 s (slow DNS, weak Wi-Fi) gets killed and restarted, so the client may
  never connect.
- `remote_webview.cpp:225-226` — `ESP_ERROR_CHECK` on `init`/`register_events`/`start`: if
  `esp_websocket_client_init()` returns `NULL` (low memory) the device aborts and boot-loops.

Fix:
- Remove `websocket_force_reconnect()` from the event handler.
- Pick one strategy: rely on built-in auto-reconnect (and drop the 5 s stop/start), **or** set
  `disable_auto_reconnect = true` and own reconnection in `rwv_ws` with backoff, triggered only after a
  disconnect has persisted longer than `network_timeout_ms`.
- Replace `ESP_ERROR_CHECK` with logged error handling and a retry/`mark_failed()`.

Docs: https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html

### C3. Dropped packets leave stale screen regions

`remote_webview.cpp:323` — when the decode queue is full the packet is freed and dropped. The server sends only
changed tiles, so a dropped tile stays stale until the next full frame (which may be never if
`full_frame_every` is not set).

Options (combine):
1. **Backpressure** — `xQueueSend(q, &m, pdMS_TO_TICKS(N))` with a meaningful timeout. Blocking the WS task
   stops reading the socket, the TCP window fills and the server slows down naturally.
2. **Recovery** — when a packet is dropped (or a tile fails to decode, see C8), request a full frame from the
   server. This needs a small protocol addition (e.g. a `RequestFullFrame` message type) on both sides.

Simply increasing queue depth only hides overload and adds latency: 12 × 60 KiB already allows ~720 KiB of
buffered payload in PSRAM.

### C4. Clipped JPEG blocks are drawn with the wrong stride

`remote_webview.cpp:525-539` (`jpeg_draw_cb_`): when the block crosses the right edge, `w` is reduced, but the
pixel buffer is passed as if rows were `w` pixels wide. Rows are actually `p->iWidth` wide, so every row after
the first is shifted — the image is skewed for multi-row blocks.

Fix: use the stride-aware overload of `Display::draw_pixels_at`:

```cpp
display_->draw_pixels_at(x, y, w, h, (const uint8_t *) p->pPixels,
                         display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, rgb565_big_endian_,
                         /*x_offset=*/0, /*y_offset=*/0, /*x_pad=*/p->iWidth - w);
```

Also handle negative `x`/`y` via `x_offset`/`y_offset`, and **(verify)** use `p->iWidthUsed` (JPEGDEC reports
MCU-aligned `iWidth`; padding pixels beyond the real image width can overwrite the neighbouring tile).

The hardware path (`remote_webview.cpp:490`) does no clipping at all: it draws `hdr.width × hdr.height` at
`dst_x, dst_y` regardless of display bounds. Apply the same clipping (stride = `aligned_w`).

### C5. Touch events

`remote_webview.cpp:609-644`

- ESPHome calls `touch()` on the first contact **and** `update()` with the point in `STATE_PRESSED`, so `Down`
  is sent twice.
- `Up` can be sent from `update()` (`RELEASING` and `RELEASED`) and again from `release()`.
- `release()` sends `Up(0, 0, id=0)` — the server may move the pointer to the top-left corner before releasing.
- **(verify)** ESPHome touch states are bit flags (`STATE_PRESSED | STATE_RELEASING`, etc.). The `switch`
  matches exact values only, so combined states fall into `default` and are ignored.
- `last_move_us_` is shared by all pointers: moves of a second finger are suppressed by the first one.

Fix: keep a small table of active pointer IDs with last known coordinates. Emit each transition exactly once:
`Down` when an ID appears, `Move` (rate-limited per ID) while present, `Up` with last known coordinates when it
disappears. Ignore `touch()`/`release()` or use them only as a fallback. Test bit flags with `&`.

ESPHome source: https://api-docs.esphome.io/touchscreen_8cpp_source

### C6. Data races between tasks

| Field | Writers | Readers |
|---|---|---|
| `url_` (`std::string`) | main loop (`open_url`) | WS event handler (`url_.c_str()` on reconnect) |
| `ws_client_` | WS event handler | main loop (touch, `open_url`), decode task (stats), `rwv_ws` |
| `last_keepalive_us_` (64-bit) | WS event handler, `rwv_ws` | `rwv_ws` |

`url_` is the dangerous one: reallocating the string while another task reads `c_str()` is a use-after-free.

Fix: protect `url_` with `state_mtx_` (copy under the lock, send outside). Make `ws_client_` an
`std::atomic<esp_websocket_client_handle_t>` or, simpler, store the handle once in `rwv_ws` and track a separate
`std::atomic<bool> connected_`. `process_frame_stats_packet_` should also check the client/connection before
sending.

### C7. Resource creation and init order

`remote_webview.cpp:77-82`

- Results of `xQueueCreate`, `xSemaphoreCreateMutex`, `xTaskCreatePinnedToCore` are not checked.
  On failure call `mark_failed()` instead of continuing in a half-initialized state.
- Tasks are started **before** the hardware JPEG decoder and its buffers are initialized (P4). The decode task
  can observe `hw_dec_`/buffers mid-initialization. Start tasks at the end of `setup()`.
- If `display_` is missing, `setup()` returns silently; prefer `mark_failed()`.

### C8. Decode failures are ignored

`remote_webview.cpp:405` — the result of `decode_jpeg_tile_to_lcd_()` is discarded and `frame_update_pending_`
is set even if every tile failed. Count failures per frame, log once per frame, request recovery (C3), and do
not fire `on_frame_update` for a failed frame (or expose the failure).

### C9. Display accessed from a non-main task

`draw_pixels_at` is called from `rwv_decode`, while ESPHome may access the same display from the main loop
(display `lambda`, `update_interval`, `show_test_card`, `auto_clear`). The README examples use
`update_interval: never`, which is safe, but this is an undocumented requirement. Document it in README (or
enforce it in `__init__.py` by validating the display config).

### C10. Minor

- `REMOTE_WEBVIEW_HAS_CACHE_MSYNC` / `esp_cache.h` are unused (`remote_webview.h:24-29`).
- `espressif/esp-dsp` is declared in `idf_component.yml` but not referenced by this component; remove it if
  the JPEGDEC fork does not need it.
- `remote_webview.cpp:715` — fallback writes an ASCII string into the MAC byte array via `snprintf`;
  generate random bytes directly instead.
- `device_id` and other query values are not URL-encoded in `build_ws_uri_()`.
- **(verify)** HW decoder always outputs a fixed RGB565 byte order, but `rgb565_big_endian_` is passed to
  `draw_pixels_at` on that path too. Confirm colors are correct with both `big_endian` settings.
- `ESP_LOGW` per tile on the HW fallback paths (`:459`, `:464`) can flood the log; log once.

---

## Performance

### P1. Build with `-O2` instead of `-Os`

README and examples set `COMPILER_OPTIMIZATION_SIZE: y`. JPEGDEC (Huffman + IDCT + color conversion) is
CPU-bound and benefits noticeably from `CONFIG_COMPILER_OPTIMIZATION_PERF: y`. Probably the cheapest win for
the software path on S3. Measure before/after.

### P2. Disable Wi-Fi power save and tune lwIP

The examples do not set `wifi: power_save_mode: none`; ESPHome's default light sleep adds latency and lowers
throughput. Also worth experimenting with:

```yaml
sdkconfig_options:
  CONFIG_LWIP_TCP_WND_DEFAULT: "65535"
  CONFIG_LWIP_TCP_RECVMBOX_SIZE: "64"
  CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM: "64"
  CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: "y"
```

### P3. Remove logging from hot paths

ESPHome's default log level is DEBUG, so `ESP_LOGD` per frame (`remote_webview.cpp:416`) and the per-tile
warnings are actually formatted and printed. Move per-frame/per-tile logs to `ESP_LOGV` or rate-limit them.

### P4. Instrument before optimizing further

`frame_start_us_` is set when the first packet of a frame is **dequeued**, so the reported stats exclude queue
wait time and don't show full client latency. Add separate timings for:
- receive → enqueue (timestamp in `WsMsg`),
- queue wait,
- JPEG decode,
- draw (`draw_pixels_at`),
- queue high-water mark and drop count.

Report them via `FrameStats` or a debug sensor. This shows the real bottleneck (network, decode or panel) and
makes the later items measurable.

### P5. Reusable receive buffer pool

Every WS message does `heap_caps_malloc` + `free` in PSRAM (`remote_webview.cpp:304`, `:352`). Preallocate N
buffers of `max_bytes_per_msg`, hand them out from a "free" queue and return them after decode. This removes
allocator cost and PSRAM fragmentation, and gives a natural backpressure point (C3): no free buffer → wait.

### P6. Right-size the P4 hardware decoder input buffer

`remote_webview.cpp:106-111` sizes the **input** (compressed) buffer like the decompressed RGB565 frame:
480×480 → 450 KiB, while the largest message is ~60 KiB. Size it from `max_bytes_per_msg` (or
`cfg::ws_max_message_bytes`) with the required DMA alignment. The output buffer can stay frame-sized, or be
sized for the maximum tile if tiles are bounded.

### P7. Fewer copies on the P4 path

Current path: WS buffer → PSRAM reassembly buffer → `memcpy` into DMA input buffer → HW decode → output buffer →
`draw_pixels_at` (copy to framebuffer). Options:
- Reassemble directly into DMA-capable memory and decode from there **(verify** that `jpeg_decoder_process`
  accepts an unaligned input pointer for tiles inside a message).
- Double-buffer the output so tile N+1 decodes while tile N is being drawn (needs a separate draw step/task).

### P8. Software path: keep compressed data in internal RAM (experiment)

Huffman decoding reads input with poor locality; through the PSRAM cache this is slower than internal RAM. For
small messages, try reassembling into internal RAM (`MALLOC_CAP_INTERNAL`) when space allows, falling back to
PSRAM. Gain depends on tile size and cache configuration — measure with P4.

### P9. Keep the main loop non-blocking

Touch sending runs in the ESPHome main loop and can block it up to ~60 ms (10 ms mutex + 50 ms send timeout).
Push touch events to a small command queue consumed by the WS side (`rwv_ws` or a dedicated sender task). This
also fits naturally with the per-pointer state machine from C5.

### P10. Memory footprint

- `rwv_decode` has a 32 KiB stack, but `JPEGDEC` is a class member, not on the stack. Check
  `uxTaskGetStackHighWaterMark()` and shrink; internal RAM is scarce with Wi-Fi + lwIP on S3.
- `ws_buffer_size = 30 KiB`: **(verify)** where `esp_websocket_client` allocates its RX/TX buffers (internal vs
  PSRAM depending on `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`).
- Server-side tuning to measure on S3: `jpeg_quality`, `tile_size`, `full_frame_tile_count`,
  `min_frame_interval`.

---

## Suggested plan

1. **Safety and reliability**: C1, C2, C7, C6 (`url_`).
2. **Visual correctness**: C3 (backpressure + full-frame request), C8, C4.
3. **Input**: C5 + P9 (pointer state machine feeding a send queue).
4. **Measure**: P4, then try P1, P2, P3 and compare.
5. **Optimize memory/throughput** based on numbers: P5, P6, P7, P8, P10.
