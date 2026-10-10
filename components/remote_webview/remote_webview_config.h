#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

inline constexpr int decode_task_stack = 32 * 1024;
inline constexpr int ws_task_stack = 8 * 1024;
inline constexpr int ws_task_prio = 5;
inline constexpr int decode_queue_depth = 12;

inline constexpr size_t ws_max_message_bytes = 64 * 1024;
inline constexpr size_t ws_buffer_size = 30 * 1024;
inline constexpr size_t ws_keepalive_interval_us = 60 * 1000 * 1000;
inline constexpr int ws_reconnect_timeout_ms = 2000;
inline constexpr int ws_network_timeout_ms = 10000;
inline constexpr int ws_init_retry_ms = 5000;
// Restart the client if auto-reconnect has not succeeded for this long.
inline constexpr uint32_t ws_restart_after_us = 60u * 1000u * 1000u;

// Minimum interval between full-frame requests while the screen is out of sync.
inline constexpr int64_t full_frame_request_interval_us = 2 * 1000 * 1000;

inline constexpr bool coalesce_moves = true;
inline constexpr uint32_t move_rate_hz = 60;

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
