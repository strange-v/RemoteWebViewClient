#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

inline constexpr int decode_task_stack = 32 * 1024;
// Same priority as ESPHome's loopTask (also on CPU 1), so a continuous stream of frames
// can't starve the main loop and trigger the task watchdog.
inline constexpr int decode_task_prio = 1;
inline constexpr int ws_task_stack = 8 * 1024;
inline constexpr int ws_task_prio = 5;
// Kept short so a slow decoder fills the queue quickly and backpressure reaches the server
// (via TCP flow control) before stale frames pile up on the device.
inline constexpr int decode_queue_depth = 4;

inline constexpr size_t ws_max_message_bytes = 64 * 1024;
inline constexpr size_t ws_buffer_size = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;
inline constexpr int ws_reconnect_timeout_ms = 2000;
inline constexpr int ws_network_timeout_ms = 10000;
inline constexpr int ws_init_retry_ms = 5000;
// Restart the client if auto-reconnect has not succeeded for this long.
inline constexpr uint32_t ws_restart_after_us = 60u * 1000u * 1000u;

// How long the WS handler waits for space in the decode queue before dropping a message.
// While it waits the socket isn't read, so TCP flow control holds back the server, which
// skips frames while the socket has unsent data. Dropping is the last resort (it triggers
// a full-frame request). Newer esp_websocket_client versions call the handler with the
// client lock held; sends don't wait on it because __init__.py enables
// CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK.
inline constexpr int decode_enqueue_timeout_ms = 1000;
// Minimum interval between "decode queue full" log lines.
inline constexpr int64_t drop_log_interval_us = 5 * 1000 * 1000;
// Frames finishing this long after a touch Up count towards touch-to-frame latency stats.
inline constexpr int64_t touch_latency_window_us = 2 * 1000 * 1000;
// Minimum interval between full-frame requests while the screen is out of sync.
inline constexpr int64_t full_frame_request_interval_us = 2 * 1000 * 1000;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
