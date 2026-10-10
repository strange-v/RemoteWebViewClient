#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/helpers.h"
#include "JPEGDEC.h"
#include "protocol.h"
#include "remote_webview_config.h"

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #include "driver/jpeg_decode.h"
  #define REMOTE_WEBVIEW_HW_JPEG 1
#else
  #define REMOTE_WEBVIEW_HW_JPEG 0
#endif
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #include "esp_cache.h"
  #define REMOTE_WEBVIEW_HAS_CACHE_MSYNC 1
#else
  #define REMOTE_WEBVIEW_HAS_CACHE_MSYNC 0
#endif

namespace esphome {
namespace remote_webview {

class RemoteWebView : public Component {
 public:
  void set_display(display::Display *d) { display_ = d; }
  void set_touchscreen(touchscreen::Touchscreen *t) { touch_ = t; }
  void set_device_id(const std::string &s) { device_id_ = s; }
  void set_url(const std::string &s) { url_ = s; }
  void set_server(const std::string &s);
  void set_tile_size(int v) { tile_size_ = v; }
  void set_full_frame_tile_count(int v) { full_frame_tile_count_ = v; }
  void set_full_frame_area_threshold(float v) { full_frame_area_threshold_ = v; }
  void set_full_frame_every(int v) { full_frame_every_ = v; }
  void set_every_nth_frame(int v) { every_nth_frame_ = v; }
  void set_min_frame_interval(int v) { min_frame_interval_ = v; }
  void set_jpeg_quality(int v) { jpeg_quality_ = v; }
  void set_max_bytes_per_msg(int v) { max_bytes_per_msg_ = v; }
  void set_big_endian(bool v) { rgb565_big_endian_ = v; }
  void set_hw_decode(bool v) { hw_decode_ = v; }
  void set_hw_decode_timeout_ms(int v) { hw_decode_timeout_ms_ = v; }
  void set_stats_interval(uint32_t ms) { stats_interval_ms_ = ms; }
  void set_rotation(int v) { rotation_ = v; }
  void set_scale(float v) { scale_ = v; }
  void disable_touch(bool disable);
  bool open_url(const std::string &s);
  std::string get_current_url() const;
  void set_url_sensor(text_sensor::TextSensor *s) { url_sensor_ = s; }
  void add_on_frame_update_callback(std::function<void()> &&callback);
  void trigger_on_frame_update();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 private:
  struct WsMsg {
    uint8_t *buf{nullptr};
    size_t   len{0};
    void    *client{nullptr}; // opaque esp_websocket_client_handle_t
    uint32_t rx_loss_seq{0};  // value of rx_loss_seq_ when this message was enqueued
    int64_t  rx_start_us{0};  // first fragment received
    int64_t  enqueue_us{0};   // put into the decode queue
  };
  struct WsReasm {
    uint8_t *buf{nullptr};
    size_t total{0}, filled{0};
    int64_t start_us{0};
  };
  // Performance stats accumulated over one stats_interval window (decode task only).
  struct PerfStats {
    uint32_t frames{0}, full_frames{0}, tiles{0};
    uint32_t failed_tiles{0}, hw_fallback_tiles{0};
    uint64_t bytes{0};
    uint64_t total_us{0}, rx_us{0}, queue_us{0}, decode_us{0}, draw_us{0};
    uint32_t total_max_us{0};
    // touch Up -> frame drawn
    uint32_t touches{0}, touches_no_frame{0};
    uint64_t touch_first_us{0}, touch_largest_us{0};
    uint32_t touch_first_max_us{0}, touch_largest_max_us{0};
  };

  CallbackManager<void()> on_frame_update_callback_{};

  static constexpr bool     kCoalesceMoves  = cfg::coalesce_moves;
  static constexpr uint32_t kMoveRateHz     = cfg::move_rate_hz;
  static constexpr uint32_t kMoveIntervalUs = (kMoveRateHz ? (1000000u / kMoveRateHz) : 0);

  static RemoteWebView *self_;
  display::Display *display_{nullptr};
  touchscreen::Touchscreen *touch_ = nullptr;
  class RemoteWebViewTouchListener *touch_listener_ = nullptr;

  int display_width_{0};
  int display_height_{0};
  std::string url_;
  std::string server_host_;
  std::string device_id_;
  int server_port_{0};
  int tile_size_{-1};
  int full_frame_tile_count_{-1};
  float full_frame_area_threshold_{-1.0f};
  int full_frame_every_{-1};
  int every_nth_frame_{-1};
  int min_frame_interval_{-1};
  int jpeg_quality_{-1};
  int max_bytes_per_msg_{-1};
  bool rgb565_big_endian_{true};
  bool hw_decode_{true};
  int hw_decode_timeout_ms_{200};
  int rotation_{0};
  float scale_{-1.0f};
  bool touch_disabled_{false};

#if REMOTE_WEBVIEW_HW_JPEG
  jpeg_decoder_handle_t hw_dec_{nullptr};
  uint8_t *hw_decode_input_buf_{nullptr};
  uint8_t *hw_decode_output_buf_{nullptr};
  size_t hw_decode_input_size_{0};
  size_t hw_decode_output_size_{0};
#endif

  std::atomic<int64_t> last_keepalive_us_{0};  // WS client task + rwv_ws
  
  uint64_t frame_start_us_ = 0;
  uint32_t frame_id_{0xffffffffu};
  uint16_t frame_tiles_{0};
  uint16_t frame_failed_tiles_{0};
  uint16_t frame_hw_fallback_tiles_{0};  // tiles decoded in software after a HW decode fallback
  uint64_t last_hw_fallback_log_us_{0};
  size_t   frame_bytes_{0};

  // Screen sync tracking. The screen is out of sync when a message was lost on receive
  // (rx_loss_seq_ != repaired_loss_seq_) or a tile failed to decode (decode_dirty_).
  // It is repaired by a full frame that arrived and decoded without any loss.
  std::atomic<uint32_t> rx_loss_seq_{0};        // written by the WS client task
  std::atomic<uint32_t> repaired_loss_seq_{0};  // written by the decode task
  std::atomic<bool>     decode_dirty_{false};   // written by the decode task
  std::atomic<int64_t>  last_full_frame_req_us_{0};
  // decode task only
  uint32_t msg_loss_seq_{0};       // stamp of the message being processed
  uint32_t prev_msg_loss_seq_{0};  // stamp of the previously processed message
  uint32_t frame_loss_seq_{0};     // stamp the current frame must keep to count as complete
  bool     frame_complete_{true};
  bool     frame_is_full_{false};

  // Timing of the message being processed and of the current frame (decode task only).
  int64_t  msg_rx_start_us_{0};
  int64_t  msg_enqueue_us_{0};
  int64_t  msg_dequeue_us_{0};
  int64_t  ft_rx_start_us_{0};  // first byte of the frame's first message
  int64_t  ft_rx_end_us_{0};    // last message of the frame enqueued
  uint32_t ft_queue_us_{0};     // sum of queue waits of the frame's messages
  uint32_t ft_decode_us_{0};    // JPEG decoding, excluding drawing
  uint32_t ft_draw_us_{0};      // draw_pixels_at calls

  uint32_t stats_interval_ms_{0};  // 0 = stats disabled
  PerfStats st_{};
  int64_t  st_window_start_us_{0};
  uint32_t st_loss_seq_{0};
  std::atomic<uint32_t> q_hwm_{0};            // max decode queue depth, WS client task
  std::atomic<uint32_t> full_frame_reqs_{0};  // full-frame requests sent, rwv_ws
  uint32_t drops_since_log_{0};    // WS client task only
  int64_t  last_drop_log_us_{0};   // WS client task only

  // Touch-to-frame latency: the main loop stamps each touch Up sent; the decode task
  // tracks frames that started arriving after it, within cfg::touch_latency_window_us.
  std::atomic<int64_t> touch_up_us_{0};
  int64_t  tl_seen_up_us_{0};   // last touch Up picked up by the decode task
  int64_t  tl_touch_us_{0};     // touch being tracked, 0 = none
  int64_t  tl_first_us_{0};     // latency to the first frame, 0 = none yet
  int64_t  tl_largest_us_{0};   // latency to the largest frame
  size_t   tl_largest_bytes_{0};
  uint32_t frame_stats_time_{0};
  uint32_t frame_stats_count_{0};
  size_t   frame_stats_bytes_{0};

  QueueHandle_t     q_decode_{nullptr};
  SemaphoreHandle_t ws_send_mtx_{nullptr};
  TaskHandle_t      t_ws_{nullptr};
  TaskHandle_t      t_decode_{nullptr};

  // Set while connected, by the WS client task. The handle itself lives as long as the
  // component, so a loaded value stays valid; use connected_client_() / ws_send_bin_().
  std::atomic<esp_websocket_client_handle_t> ws_client_{nullptr};

  std::atomic<bool> frame_update_pending_{false};
  std::atomic<bool> url_publish_pending_{false};
  std::string pending_url_{};
  SemaphoreHandle_t state_mtx_{nullptr};

  uint32_t last_trigger_ms_{0};
  text_sensor::TextSensor *url_sensor_{nullptr};

  bool start_ws_task_();
  bool start_decode_task_();
  static void ws_task_tramp_(void *arg);
  static void decode_task_tramp_(void *arg);

  static void ws_event_handler_(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data);
  static void reasm_reset_(WsReasm &r);

  void process_packet_(void *client, const uint8_t *data, size_t len);
  void process_frame_packet_(const uint8_t *data, size_t len);
  void process_frame_stats_packet_(const uint8_t *data, size_t len);
  bool decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len);
  bool decode_jpeg_tile_software_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len);

  static int jpeg_draw_cb_s_(JPEGDRAW *p);
  int jpeg_draw_cb_(JPEGDRAW *p);
  // Draws a w×h RGB565 region whose source rows are `stride` pixels apart, clipped to
  // [0, clip_x1) × [0, clip_y1).
  void draw_rgb565_clipped_(int x, int y, int w, int h, int stride, const uint8_t *pixels,
                            int clip_x1, int clip_y1);
  JPEGDEC jd_;
  int sw_clip_x1_{0};  // clip bounds of the tile being decoded in software (decode task only)
  int sw_clip_y1_{0};

  bool ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid);
  bool ws_send_keepalive_();
  bool ws_send_open_url_(const char *url, uint16_t flags);
  bool ws_send_request_full_frame_();
  bool ws_send_bin_(const uint8_t *data, size_t len, TickType_t lock_timeout, TickType_t send_timeout);
  esp_websocket_client_handle_t connected_client_() const;

  // url_ is guarded by state_mtx_ once tasks are running.
  std::string get_url_() const;
  void set_url_(const std::string &url);

  void maybe_report_stats_();
  void touch_latency_poll_(int64_t now);
  void touch_latency_finish_();
  bool screen_out_of_sync_() const;
  void note_rx_loss_();
  void note_decode_failure_();
  void notify_ws_task_();

  std::string resolve_device_id_() const;
  std::string build_ws_uri_() const;
  static void append_q_int_(std::string &s, const char *k, int v);
  static void append_q_float_(std::string &s, const char *k, float v);
  static void append_q_str_(std::string &s, const char *k, const char *v);

  void process_current_url_packet_(const uint8_t *data, size_t len);

  friend class RemoteWebViewTouchListener;
};

class RemoteWebViewTouchListener : public touchscreen::TouchListener {
 public:
  explicit RemoteWebViewTouchListener(RemoteWebView *p) : parent_(p) {}
  void touch(touchscreen::TouchPoint tp) override;
  void update(const touchscreen::TouchPoints_t &pts) override;
  void release() override;
 private:
  void begin_(const touchscreen::TouchPoint &p);
  void move_to_(int x, int y);
  void end_();

  RemoteWebView *parent_{nullptr};
  // Primary pointer state; accessed from the ESPHome main loop only.
  bool     active_{false};
  bool     wait_all_up_{false};  // ignore touches until every finger is lifted
  bool     move_pending_{false}; // a throttled move hasn't been sent yet
  uint8_t  active_id_{0};
  int      last_x_{0};
  int      last_y_{0};
  uint64_t last_move_us_{0};
};

}  // namespace remote_webview
}  // namespace esphome
