#include "remote_webview.h"
#include "remote_webview_config.h"
#include "esphome/core/log.h"

#include "esp_idf_version.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_efuse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>

namespace esphome {
namespace remote_webview {

static const char *const TAG = "Remote_WebView";
RemoteWebView *RemoteWebView::self_ = nullptr;

//Below two functions are part of automation template testing
void RemoteWebView::add_on_frame_update_callback(std::function<void()> &&callback) {
  this->on_frame_update_callback_.add(std::move(callback));
}
void RemoteWebView::trigger_on_frame_update() {
  uint32_t now = millis();
  
  if (now - this->last_trigger_ms_ < 1000) return;
  
  this->last_trigger_ms_ = now;
  
  ESP_LOGD(TAG, "Triggering the on_frame_update automation");
  this->on_frame_update_callback_.call();
}

void RemoteWebView::process_current_url_packet_(const uint8_t *data, size_t len) {
  if (!data || len < sizeof(proto::CurrentURLHeader)) return;
  
  auto *hdr = reinterpret_cast<const proto::CurrentURLHeader *>(data);
  if (hdr->url_len > len - sizeof(proto::CurrentURLHeader)) return;
  
  if (this->url_sensor_ == nullptr) return;

  std::string url(reinterpret_cast<const char*>(data + sizeof(proto::CurrentURLHeader)), hdr->url_len);
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    this->pending_url_ = std::move(url);
    this->url_publish_pending_.store(true, std::memory_order_release);
    xSemaphoreGive(this->state_mtx_);
  }
}

std::string RemoteWebView::get_current_url() const {
  if (this->url_sensor_ != nullptr && this->url_sensor_->has_state()) {
    return this->url_sensor_->state;
  }
  return "";
}

void RemoteWebView::setup() {
  self_ = this;

  if (!display_) {
    ESP_LOGE(TAG, "no display");
    this->mark_failed();
    return;
  }

  display_width_ = display_->get_width();
  display_height_ = display_->get_height();

  q_decode_ = xQueueCreate(cfg::decode_queue_depth, sizeof(WsMsg));
  ws_send_mtx_ = xSemaphoreCreateMutex();
  state_mtx_ = xSemaphoreCreateMutex();
  if (!q_decode_ || !ws_send_mtx_ || !state_mtx_) {
    ESP_LOGE(TAG, "failed to create queue/mutexes");
    this->mark_failed();
    return;
  }

#if REMOTE_WEBVIEW_HW_JPEG
  if (this->hw_decode_) {
    jpeg_decode_engine_cfg_t jcfg = {
      .timeout_ms = this->hw_decode_timeout_ms_,
    };
    if (jpeg_new_decoder_engine(&jcfg, &hw_dec_) != ESP_OK) {
      hw_dec_ = nullptr;
    }
  }

  if (this->hw_dec_) {
    const int W = display_->get_width();
    const int H = display_->get_height();
    const int aligned_w = (W + 15) & ~15;
    const int aligned_h = (H + 15) & ~15;
    
    const size_t max_buffer_size = (size_t)aligned_w * (size_t)aligned_h * 2u;
    
    jpeg_decode_memory_alloc_cfg_t in_cfg { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    jpeg_decode_memory_alloc_cfg_t out_cfg { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    
    hw_decode_input_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)max_buffer_size, &in_cfg, &hw_decode_input_size_);
    hw_decode_output_buf_ = (uint8_t*)jpeg_alloc_decoder_mem((uint32_t)max_buffer_size, &out_cfg, &hw_decode_output_size_);
    
    if (!hw_decode_input_buf_ || !hw_decode_output_buf_) {
      ESP_LOGE(TAG, "Failed to allocate HW decoder buffers");
      if (hw_decode_input_buf_) free(hw_decode_input_buf_);
      if (hw_decode_output_buf_) free(hw_decode_output_buf_);
      hw_decode_input_buf_ = nullptr;
      hw_decode_output_buf_ = nullptr;
      jpeg_del_decoder_engine(hw_dec_);
      hw_dec_ = nullptr;
    } else {
      ESP_LOGD(TAG, "HW decoder buffers allocated: input=%u, output=%u",
               (unsigned)hw_decode_input_size_, (unsigned)hw_decode_output_size_);
    }
  }
#endif

  // Start tasks only after everything they use (incl. HW decoder) is initialized.
  if (!start_decode_task_() || !start_ws_task_()) {
    ESP_LOGE(TAG, "failed to create tasks");
    this->mark_failed();
    return;
  }

  if (touch_) {
    touch_listener_ = new RemoteWebViewTouchListener(this);
    touch_->register_listener(touch_listener_);
    ESP_LOGD(TAG, "touch listener registered");
  }
}

void RemoteWebView::loop() {
  if (this->frame_update_pending_.exchange(false, std::memory_order_acq_rel)) {
    this->trigger_on_frame_update();
  }

  if (!this->url_sensor_) return;
  if (!this->url_publish_pending_.exchange(false, std::memory_order_acq_rel)) return;

  std::string url;
  if (this->state_mtx_ && xSemaphoreTake(this->state_mtx_, pdMS_TO_TICKS(10)) == pdTRUE) {
    url = this->pending_url_;
    xSemaphoreGive(this->state_mtx_);
  }

  if (!url.empty()) {
    this->url_sensor_->publish_state(url);
    ESP_LOGD(TAG, "Current Server URL updated: %s", url.c_str());
  }
}

void RemoteWebView::dump_config() {
  ESP_LOGCONFIG(TAG, "remote_webview:");

  const std::string id = device_id_.empty() ? resolve_device_id_() : device_id_;
  ESP_LOGCONFIG(TAG, "  id: %s", id.c_str());

  if (display_) {
    ESP_LOGCONFIG(TAG, "  display: %dx%d", display_->get_width(), display_->get_height());
  }

#if REMOTE_WEBVIEW_HW_JPEG
  ESP_LOGCONFIG(TAG, "  hw_decode: %s", this->hw_decode_ ? "true" : "false");
  if (this->hw_decode_)
    ESP_LOGCONFIG(TAG, "  hw_decode_timeout_ms: %d", this->hw_decode_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  hw_jpeg_available: %s", hw_dec_ ? "yes" : "no");
#else
  ESP_LOGCONFIG(TAG, "  hw_jpeg: no");
#endif

  if (stats_interval_ms_)
    ESP_LOGCONFIG(TAG, "  stats_interval: %u ms", (unsigned)stats_interval_ms_);
  ESP_LOGCONFIG(TAG, "  server: %s:%d", server_host_.c_str(), server_port_);
  ESP_LOGCONFIG(TAG, "  url: %s", get_url_().c_str());

  auto print_opt_int = [&](const char *name, int v) {
    if (v >= 0) ESP_LOGCONFIG(TAG, "  %s: %d", name, v);
  };
  auto print_opt_float2 = [&](const char *name, float v) {
    if (v >= 0.0f) ESP_LOGCONFIG(TAG, "  %s: %.2f", name, (double)v);
  };

  print_opt_int   ("tile_size",                 tile_size_);
  print_opt_int   ("full_frame_tile_count",     full_frame_tile_count_);
  print_opt_float2("full_frame_area_threshold", full_frame_area_threshold_);
  print_opt_int   ("full_frame_every",          full_frame_every_);
  print_opt_int   ("every_nth_frame",           every_nth_frame_);
  print_opt_int   ("min_frame_interval",        min_frame_interval_);
  print_opt_int   ("jpeg_quality",              jpeg_quality_);
  print_opt_int   ("max_bytes_per_msg",         max_bytes_per_msg_);
  print_opt_int   ("big_endian",                rgb565_big_endian_);
  print_opt_int   ("rotation",                  rotation_);
}

bool RemoteWebView::open_url(const std::string &s) {
  if (s.empty()) return false;
  
  if (ws_send_open_url_(s.c_str(), 0)) {
    set_url_(s);
    ESP_LOGD(TAG, "opened URL: %s", s.c_str());
    return true;
  }
  
  return false;
}

bool RemoteWebView::start_ws_task_() {
  return xTaskCreatePinnedToCore(&RemoteWebView::ws_task_tramp_, "rwv_ws", cfg::ws_task_stack, this, 5, &t_ws_, 0) == pdPASS;
}

void RemoteWebView::ws_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);

  std::string uri_str = self->build_ws_uri_();
  esp_websocket_client_config_t cfg_ws = {};
  cfg_ws.uri = uri_str.c_str();
  cfg_ws.reconnect_timeout_ms = cfg::ws_reconnect_timeout_ms;
  cfg_ws.network_timeout_ms   = cfg::ws_network_timeout_ms;
  cfg_ws.task_stack           = cfg::ws_task_stack;
  cfg_ws.task_prio            = cfg::ws_task_prio;
  cfg_ws.buffer_size          = cfg::ws_buffer_size;
  cfg_ws.disable_auto_reconnect = false;

  WsReasm reasm{};
  esp_websocket_client_handle_t client = nullptr;
  for (;;) {
    client = esp_websocket_client_init(&cfg_ws);
    if (client) {
      esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler_, &reasm);
      if (err == ESP_OK) err = esp_websocket_client_start(client);
      if (err == ESP_OK) break;
      ESP_LOGE(TAG, "[ws] client start failed: %s", esp_err_to_name(err));
      esp_websocket_client_destroy(client);
      client = nullptr;
    } else {
      ESP_LOGE(TAG, "[ws] client init failed");
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::ws_init_retry_ms));
  }

  // Reconnects are handled by the client's built-in auto-reconnect. This loop only restarts
  // the client if it has been stuck disconnected for much longer than a normal reconnect cycle.
  int64_t connected_seen_us = esp_timer_get_time();
  for (;;) {
    // Woken early by notify_ws_task_() when the screen goes out of sync.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const int64_t now_us = esp_timer_get_time();

    if (!esp_websocket_client_is_connected(client)) {
      if (now_us - connected_seen_us >= (int64_t)cfg::ws_restart_after_us) {
        ESP_LOGW(TAG, "[ws] not connected for %u s, restarting client",
                 (unsigned)(cfg::ws_restart_after_us / 1000000));
        esp_websocket_client_stop(client);
        const esp_err_t err = esp_websocket_client_start(client);
        if (err != ESP_OK)
          ESP_LOGE(TAG, "[ws] client restart failed: %s", esp_err_to_name(err));
        connected_seen_us = now_us;
      }
      continue;
    }
    connected_seen_us = now_us;

    if (self && self->connected_client_()) {
      if (now_us - self->last_keepalive_us_.load() >= (int64_t)cfg::ws_keepalive_interval_us) {
        if (self->ws_send_keepalive_()) {
          self->last_keepalive_us_.store(now_us);
          ESP_LOGV(TAG, "[ws] keepalive sent");
        }
      }

      if (self->screen_out_of_sync_() &&
          now_us - self->last_full_frame_req_us_.load() >= cfg::full_frame_request_interval_us) {
        if (self->ws_send_request_full_frame_()) {
          self->last_full_frame_req_us_.store(now_us);
          self->full_frame_reqs_.fetch_add(1);
          ESP_LOGD(TAG, "[ws] screen out of sync, full frame requested");
        }
      }
    }
  }
}

void RemoteWebView::reasm_reset_(WsReasm &r) {
  if (r.buf) free(r.buf);
  r.buf = nullptr; r.total = 0; r.filled = 0;
}

void RemoteWebView::ws_event_handler_(void *handler_arg, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *r = reinterpret_cast<WsReasm*>(handler_arg);
  auto *e = reinterpret_cast<const esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "[ws] connected");
      if (self_) {
        self_->ws_client_.store(e->client);
        self_->last_keepalive_us_.store(esp_timer_get_time());
        // The server sends a full frame on connect; don't request another one right away.
        self_->last_full_frame_req_us_.store(esp_timer_get_time());
        const std::string url = self_->get_url_();
        if (!url.empty())
          self_->ws_send_open_url_(url.c_str(), 0);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      if (self_) self_->ws_client_.store(nullptr);
      ESP_LOGI(TAG, "[ws] disconnected");
      if (self_) self_->last_keepalive_us_.store(0);
      if (r->buf && self_) self_->note_rx_loss_();
      reasm_reset_(*r);
      // Auto-reconnect is enabled; esp_websocket_client_stop() must not be called from this handler.
      break;

#ifdef WEBSOCKET_EVENT_CLOSED
    case WEBSOCKET_EVENT_CLOSED:
      if (self_) self_->ws_client_.store(nullptr);
      ESP_LOGI(TAG, "[ws] closed");
      if (self_) self_->last_keepalive_us_.store(0);
      if (r->buf && self_) self_->note_rx_loss_();
      reasm_reset_(*r);
      break;
#endif

    case WEBSOCKET_EVENT_DATA: {
      if (!self_) break;

      const uint8_t *frag = (const uint8_t *)e->data_ptr;
      size_t frag_len = (size_t)e->data_len;
      bool is_bin  = (e->op_code == WS_TRANSPORT_OPCODES_BINARY);
      if (!is_bin) break;

      // Every message that doesn't reach the decode queue is counted as a loss (note_rx_loss_),
      // which triggers a full-frame request. Later fragments of a dropped message are skipped
      // by the `!r->buf` check below and are not counted again.
      if (e->payload_offset == 0) {
        if (r->buf) {
          ESP_LOGE(TAG, "incomplete WS message discarded");
          self_->note_rx_loss_();
        }
        reasm_reset_(*r);
        r->start_us = esp_timer_get_time();
        const size_t max_allowed = (self_ && self_->max_bytes_per_msg_ > 0)
                                   ? (size_t)self_->max_bytes_per_msg_
                                   : cfg::ws_max_message_bytes;
        if ((size_t)e->payload_len > max_allowed) {
          ESP_LOGE(TAG, "WS message too large: %u > %u", (unsigned)e->payload_len, (unsigned)max_allowed);
          self_->note_rx_loss_();
          break;
        }
        r->total = (size_t)e->payload_len;
        r->buf   = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->buf) r->buf = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_8BIT);
        if (!r->buf) {
          ESP_LOGE(TAG, "malloc %u failed", (unsigned)r->total);
          r->total = 0;
          self_->note_rx_loss_();
          break;
        }
      }
      if (!r->buf || r->total == 0) break;

      if ((size_t)e->payload_offset + frag_len > r->total) {
        ESP_LOGE(TAG, "bad fragment bounds");
        reasm_reset_(*r);
        self_->note_rx_loss_();
        break;
      }
      memcpy(r->buf + e->payload_offset, frag, frag_len);
      size_t new_filled = (size_t)e->payload_offset + frag_len;
      if (new_filled > r->filled) r->filled = new_filled;

      if (r->filled == r->total) {
        WsMsg m;
        m.buf = r->buf; m.len = r->total; m.client = e->client;
        m.rx_loss_seq = self_->rx_loss_seq_.load();
        m.rx_start_us = r->start_us;
        m.enqueue_us = esp_timer_get_time();
        r->buf = nullptr; r->total = 0; r->filled = 0;
        if (!self_->q_decode_ ||
            xQueueSend(self_->q_decode_, &m, pdMS_TO_TICKS(cfg::decode_enqueue_timeout_ms)) != pdTRUE) {
          free(m.buf);
          self_->note_rx_loss_();
          self_->drops_since_log_++;
          const int64_t now = esp_timer_get_time();
          if (now - self_->last_drop_log_us_ >= cfg::drop_log_interval_us) {
            ESP_LOGE(TAG, "decode queue full, dropped %u packet(s)", (unsigned)self_->drops_since_log_);
            self_->drops_since_log_ = 0;
            self_->last_drop_log_us_ = now;
          }
        } else {
          const uint32_t depth = (uint32_t)uxQueueMessagesWaiting(self_->q_decode_);
          if (depth > self_->q_hwm_.load()) self_->q_hwm_.store(depth);
        }
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "[ws] error: type=%d tls_err=%d tls_stack=%d",
               e->error_handle.error_type,
               e->error_handle.esp_tls_last_esp_err,
               e->error_handle.esp_tls_stack_err);
      break;

    default: break;
  }
}

bool RemoteWebView::start_decode_task_() {
  return xTaskCreatePinnedToCore(&RemoteWebView::decode_task_tramp_, "rwv_decode", cfg::decode_task_stack, this, cfg::decode_task_prio, &t_decode_, 1) == pdPASS;
}

void RemoteWebView::decode_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);
  WsMsg m;
  // With stats enabled, wake up periodically so a window is reported even when idle.
  const TickType_t wait = self->stats_interval_ms_ ? pdMS_TO_TICKS(self->stats_interval_ms_) : portMAX_DELAY;
  for (;;) {
    if (xQueueReceive(self->q_decode_, &m, wait) == pdTRUE) {
      self->msg_loss_seq_ = m.rx_loss_seq;
      self->msg_rx_start_us_ = m.rx_start_us;
      self->msg_enqueue_us_ = m.enqueue_us;
      self->msg_dequeue_us_ = esp_timer_get_time();
      self->process_packet_(m.client, m.buf, m.len);
      self->prev_msg_loss_seq_ = m.rx_loss_seq;
      free(m.buf);
    }
    if (self->stats_interval_ms_)
      self->maybe_report_stats_();
  }
}

void RemoteWebView::touch_latency_poll_(int64_t now) {
  const int64_t up = touch_up_us_.load();
  if (up != tl_seen_up_us_) {
    // New touch: close the previous one and start tracking this one.
    if (tl_touch_us_) touch_latency_finish_();
    tl_seen_up_us_ = up;
    tl_touch_us_ = up;
    tl_first_us_ = 0;
    tl_largest_us_ = 0;
    tl_largest_bytes_ = 0;
  } else if (tl_touch_us_ && now - tl_touch_us_ >= cfg::touch_latency_window_us) {
    touch_latency_finish_();
  }
}

void RemoteWebView::touch_latency_finish_() {
  st_.touches++;
  if (tl_first_us_) {
    st_.touch_first_us += (uint64_t)tl_first_us_;
    st_.touch_largest_us += (uint64_t)tl_largest_us_;
    st_.touch_first_max_us = std::max(st_.touch_first_max_us, (uint32_t)tl_first_us_);
    st_.touch_largest_max_us = std::max(st_.touch_largest_max_us, (uint32_t)tl_largest_us_);
  } else {
    st_.touches_no_frame++;
  }
  tl_touch_us_ = 0;
}

void RemoteWebView::maybe_report_stats_() {
  const int64_t now = esp_timer_get_time();
  touch_latency_poll_(now);
  if (st_window_start_us_ == 0) {
    st_window_start_us_ = now;
    st_loss_seq_ = rx_loss_seq_.load();
    return;
  }
  const int64_t elapsed_us = now - st_window_start_us_;
  if (elapsed_us < (int64_t)stats_interval_ms_ * 1000) return;

  const uint32_t loss_seq = rx_loss_seq_.load();
  const uint32_t dropped = loss_seq - st_loss_seq_;
  const uint32_t q_max = q_hwm_.exchange(0);
  const uint32_t reqs = full_frame_reqs_.exchange(0);
  const PerfStats s = st_;
  st_ = PerfStats{};
  st_window_start_us_ = now;
  st_loss_seq_ = loss_seq;

  if (s.frames == 0 && dropped == 0 && reqs == 0 && s.touches == 0) return;  // idle window

  const float secs = (float)elapsed_us / 1e6f;
  const float n = s.frames ? (float)s.frames : 1.0f;
  auto avg_ms = [n](uint64_t us) { return (float)us / n / 1000.0f; };

  ESP_LOGI(TAG, "stats %.1f s: %u frames (%.1f fps, %u full), %u tiles, %.1f KiB/s",
           secs, (unsigned)s.frames, s.frames / secs, (unsigned)s.full_frames, (unsigned)s.tiles,
           (float)s.bytes / 1024.0f / secs);
  ESP_LOGI(TAG, "  per frame avg: total %.1f ms (max %.1f), rx %.1f, queue %.1f, decode %.1f, draw %.1f ms",
           avg_ms(s.total_us), s.total_max_us / 1000.0f, avg_ms(s.rx_us), avg_ms(s.queue_us),
           avg_ms(s.decode_us), avg_ms(s.draw_us));
  ESP_LOGI(TAG, "  queue max %u/%d, dropped %u, failed tiles %u, hw fallback tiles %u, full-frame requests %u",
           (unsigned)q_max, cfg::decode_queue_depth, (unsigned)dropped, (unsigned)s.failed_tiles,
           (unsigned)s.hw_fallback_tiles, (unsigned)reqs);

  if (s.touches) {
    const uint32_t answered = s.touches - s.touches_no_frame;
    const float a = answered ? (float)answered : 1.0f;
    ESP_LOGI(TAG, "  touch->frame: %u touches, first avg %.1f ms (max %.1f), largest avg %.1f ms (max %.1f), no frame %u",
             (unsigned)s.touches, s.touch_first_us / a / 1000.0f, s.touch_first_max_us / 1000.0f,
             s.touch_largest_us / a / 1000.0f, s.touch_largest_max_us / 1000.0f,
             (unsigned)s.touches_no_frame);
  }
}

void RemoteWebView::process_packet_(void * /*client*/, const uint8_t *data, size_t len) {
  if (!data || len == 0) return;

  const proto::MsgType type = (proto::MsgType)data[0];
  switch (type) {
    case proto::MsgType::Frame:
      process_frame_packet_(data, len);
      break;
    case proto::MsgType::FrameStats:
      process_frame_stats_packet_(data, len);
      break;
    case proto::MsgType::CurrentURL: // deal with packet #6 here - aka our current display URL packet
      process_current_url_packet_(data, len);
      break;
    default:
      ESP_LOGW(TAG, "unknown packet type: %d", (int)type);
      break;
  }
}

void RemoteWebView::process_frame_packet_(const uint8_t *data, size_t len)
{
  proto::FrameInfo fi{};
  size_t off = 0;
  if (!data || !proto::parse_frame_header(data, len, fi, off)) {
    ESP_LOGE(TAG, "invalid frame header (len=%u)", (unsigned)len);
    frame_complete_ = false;
    note_decode_failure_();
    return;
  }

  if (fi.frame_id != frame_id_) {
    frame_id_ = fi.frame_id;
    frame_tiles_= 0;
    frame_failed_tiles_ = 0;
    frame_hw_fallback_tiles_ = 0;
    frame_bytes_= 0;
    frame_start_us_ = esp_timer_get_time();
    frame_is_full_ = (fi.flags & proto::kFlagIsFullFrame) != 0;
    frame_loss_seq_ = prev_msg_loss_seq_;
    frame_complete_ = true;
    ft_rx_start_us_ = msg_rx_start_us_;
    ft_queue_us_ = 0;
    ft_decode_us_ = 0;
    ft_draw_us_ = 0;
  }
  ft_rx_end_us_ = msg_enqueue_us_;
  ft_queue_us_ += (uint32_t)(msg_dequeue_us_ - msg_enqueue_us_);
  // A message lost right before this one may belong to this frame (even its first chunk),
  // so the frame can't be trusted to repair the screen.
  if (msg_loss_seq_ != frame_loss_seq_) frame_complete_ = false;

  frame_bytes_ += len;
  frame_tiles_ += fi.tile_count;

  uint16_t failed = 0;
  uint16_t i = 0;
  for (; i < fi.tile_count; i++) {
    proto::TileHeader th{};
    if (!proto::parse_tile_header(data, len, th, off)) break;
    if (th.dlen > len - off) break;

    const bool valid = th.w != 0 && th.h != 0 && th.dlen != 0 &&
                       th.w <= display_width_ && th.h <= display_height_ &&
                       th.x < display_width_ && th.y < display_height_;
    bool ok = valid && fi.enc == proto::Encoding::JPEG;
    if (ok) {
      const int64_t t0 = esp_timer_get_time();
      const uint32_t draw0 = ft_draw_us_;
      ok = decode_jpeg_tile_to_lcd_((int16_t)th.x, (int16_t)th.y, data + off, th.dlen);
      ft_decode_us_ += (uint32_t)(esp_timer_get_time() - t0) - (ft_draw_us_ - draw0);
    }
    if (!ok) failed++;

    off += th.dlen;
  }
  if (i < fi.tile_count) {
    ESP_LOGE(TAG, "frame %lu: truncated packet, parsed %u of %u tiles", frame_id_, i, fi.tile_count);
    failed += fi.tile_count - i;
  }
  if (failed) {
    frame_failed_tiles_ += failed;
    frame_complete_ = false;
    note_decode_failure_();
  }

  if (fi.flags & proto::kFlafLastOfFrame) {
    // "Done with this frame" (drawn or not), sent first so the server can pace the next one.
    // Short timeouts keep decoding from blocking; a lost ack is covered by the next one
    // (acks are cumulative) or by the server's timeout.
    uint8_t ack[sizeof(proto::FrameAckPacket)];
    const size_t ack_len = proto::build_frame_ack_packet(fi.frame_id, ack);
    ws_send_bin_(ack, ack_len, pdMS_TO_TICKS(10), pdMS_TO_TICKS(50));

    const uint32_t time_ms = (esp_timer_get_time() - frame_start_us_) / 1000ULL;
    frame_stats_bytes_ += frame_bytes_;
    frame_stats_time_ += time_ms;
    frame_stats_count_++;
    ESP_LOGV(TAG, "frame %lu: tiles %u (%u bytes) - %lu ms", frame_id_, frame_tiles_, frame_bytes_, time_ms);

    if (frame_failed_tiles_)
      ESP_LOGW(TAG, "frame %lu: %u of %u tiles failed", frame_id_, frame_failed_tiles_, frame_tiles_);

    if (stats_interval_ms_) {
      const uint32_t total_us = (uint32_t)(esp_timer_get_time() - ft_rx_start_us_);
      st_.frames++;
      if (frame_is_full_) st_.full_frames++;
      st_.tiles += frame_tiles_;
      st_.bytes += frame_bytes_;
      st_.failed_tiles += frame_failed_tiles_;
      st_.hw_fallback_tiles += frame_hw_fallback_tiles_;
      st_.total_us += total_us;
      if (total_us > st_.total_max_us) st_.total_max_us = total_us;
      st_.rx_us += (uint64_t)(ft_rx_end_us_ - ft_rx_start_us_);
      st_.queue_us += ft_queue_us_;
      st_.decode_us += ft_decode_us_;
      st_.draw_us += ft_draw_us_;

      const int64_t now = esp_timer_get_time();
      touch_latency_poll_(now);
      // Only frames that started arriving after the touch can be its response.
      if (tl_touch_us_ && ft_rx_start_us_ >= tl_touch_us_) {
        const int64_t latency = now - tl_touch_us_;
        if (!tl_first_us_) tl_first_us_ = latency;
        if (frame_bytes_ > tl_largest_bytes_) {
          tl_largest_bytes_ = frame_bytes_;
          tl_largest_us_ = latency;
        }
      }
    }

    // Per-tile reasons are logged at VERBOSE; warn about software fallback at most every 10 s.
    if (frame_hw_fallback_tiles_ && frame_start_us_ - last_hw_fallback_log_us_ >= 10ULL * 1000 * 1000) {
      last_hw_fallback_log_us_ = frame_start_us_;
      ESP_LOGW(TAG, "frame %lu: %u of %u tiles fell back to software JPEG decoding",
               frame_id_, frame_hw_fallback_tiles_, frame_tiles_);
    }

    if (frame_is_full_ && frame_complete_) {
      // Repairs every loss stamped up to the start of this frame. Losses after it keep
      // rx_loss_seq_ ahead of repaired_loss_seq_ and trigger another request.
      decode_dirty_.store(false);
      repaired_loss_seq_.store(frame_loss_seq_);
    }

    if (frame_failed_tiles_ == 0)
      this->frame_update_pending_.store(true, std::memory_order_release);
  }
}

bool RemoteWebView::screen_out_of_sync_() const {
  return decode_dirty_.load() || rx_loss_seq_.load() != repaired_loss_seq_.load();
}

void RemoteWebView::note_rx_loss_() {
  rx_loss_seq_.fetch_add(1);
  notify_ws_task_();
}

void RemoteWebView::note_decode_failure_() {
  decode_dirty_.store(true);
  notify_ws_task_();
}

void RemoteWebView::notify_ws_task_() {
  if (t_ws_) xTaskNotifyGive(t_ws_);
}

void RemoteWebView::process_frame_stats_packet_(const uint8_t *data, size_t len)
{
  uint32_t avg_render_time = 0;
  if (frame_stats_count_ > 0)
    avg_render_time = frame_stats_time_ / frame_stats_count_;

  ESP_LOGD(TAG, "sending frame stats: avg_time=%u ms, bytes=%u", (unsigned)avg_render_time, (unsigned)frame_stats_bytes_);
  uint8_t pkt[sizeof(proto::FrameStatsPacket)];
  const size_t n = proto::build_frame_stats_packet(avg_render_time, frame_stats_bytes_, pkt);

  frame_stats_time_ = 0;
  frame_stats_count_ = 0;
  frame_stats_bytes_ = 0;

  ws_send_bin_(pkt, n, pdMS_TO_TICKS(50), pdMS_TO_TICKS(50));
}

bool RemoteWebView::decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!data || !len) return false;

#if REMOTE_WEBVIEW_HW_JPEG
  if (hw_dec_ && hw_decode_input_buf_ && hw_decode_output_buf_) {
    jpeg_decode_picture_info_t hdr{};
    if (jpeg_decoder_get_info(data, (uint32_t)len, &hdr) != ESP_OK || !hdr.width || !hdr.height) {
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    const int aligned_w = (hdr.width  + 15) & ~15;
    const int aligned_h = (hdr.height + 15) & ~15;
    const uint32_t out_sz = (uint32_t)aligned_w * (uint32_t)aligned_h * 2u;

    if (aligned_w != (int)hdr.width) {
      ESP_LOGV(TAG, "HW fallback: jpeg dimensions not aligned: %u x %u", (unsigned)hdr.width, (unsigned)hdr.height);
      frame_hw_fallback_tiles_++;
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }
    
    if (len > hw_decode_input_size_ || out_sz > hw_decode_output_size_) {
      ESP_LOGV(TAG, "HW fallback: tile too large for HW decoder buffers");
      frame_hw_fallback_tiles_++;
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    jpeg_decode_cfg_t jcfg{};
    jcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    jcfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    jcfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT709;

    memcpy(hw_decode_input_buf_, data, len);
    
    uint32_t written = 0;
    esp_err_t dr = jpeg_decoder_process(hw_dec_, &jcfg, hw_decode_input_buf_, (uint32_t)len, 
                                        hw_decode_output_buf_, out_sz, &written);

    if (dr != ESP_OK) {
      ESP_LOGV(TAG, "HW fallback: decode failed: %s", esp_err_to_name(dr));
      frame_hw_fallback_tiles_++;
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    const uint32_t visible_sz = (uint32_t)hdr.width * (uint32_t)hdr.height * 2u;
    if (written < visible_sz) {
      ESP_LOGV(TAG, "HW fallback: incomplete decode: expected at least %u bytes, got %u",
               (unsigned)visible_sz, (unsigned)written);
      frame_hw_fallback_tiles_++;
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    draw_rgb565_clipped_(dst_x, dst_y, (int)hdr.width, (int)hdr.height, aligned_w, hw_decode_output_buf_,
                         std::min<int>(dst_x + (int)hdr.width, display_width_),
                         std::min<int>(dst_y + (int)hdr.height, display_height_));

    return true;
  }
#endif  // REMOTE_WEBVIEW_HW_JPEG

  return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
}

bool RemoteWebView::decode_jpeg_tile_software_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!jd_.openRAM((uint8_t*)data, (int)len, &RemoteWebView::jpeg_draw_cb_s_)) {
    ESP_LOGE(TAG, "openRAM failed (len=%u) err=%d", (unsigned)len, jd_.getLastError());
    return false;
  }

  jd_.setMaxOutputSize(8 * 2048);
  jd_.setPixelType(rgb565_big_endian_ ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);

  // Blocks are MCU-aligned and may extend past the image; clip them to the tile and the display.
  sw_clip_x1_ = std::min<int>(dst_x + jd_.getWidth(), display_width_);
  sw_clip_y1_ = std::min<int>(dst_y + jd_.getHeight(), display_height_);

  const int rc = jd_.decode(dst_x, dst_y, 0);
  if (rc == 0) {
    ESP_LOGE(TAG, "decode rc=%d err=%d", rc, jd_.getLastError());
    jd_.close();
    return false;
  }
  jd_.close();
  return true;
}

int RemoteWebView::jpeg_draw_cb_s_(JPEGDRAW *p) {
  return self_ ? self_->jpeg_draw_cb_(p) : 0;
}

int RemoteWebView::jpeg_draw_cb_(JPEGDRAW *p) {
  // Rows in pPixels are p->iWidth pixels apart, even when only part of the block is drawn.
  draw_rgb565_clipped_(p->x, p->y, p->iWidth, p->iHeight, p->iWidth, (const uint8_t *)p->pPixels,
                       sw_clip_x1_, sw_clip_y1_);
  return 1;
}

void RemoteWebView::draw_rgb565_clipped_(int x, int y, int w, int h, int stride, const uint8_t *pixels,
                                         int clip_x1, int clip_y1) {
  int x_off = 0, y_off = 0;
  if (x < 0) { x_off = -x; w += x; x = 0; }
  if (y < 0) { y_off = -y; h += y; y = 0; }
  if (x + w > clip_x1) w = clip_x1 - x;
  if (y + h > clip_y1) h = clip_y1 - y;
  if (w <= 0 || h <= 0) return;

  const int64_t t0 = esp_timer_get_time();
  display_->draw_pixels_at(x, y, w, h, pixels,
                           esphome::display::COLOR_ORDER_RGB,
                           esphome::display::COLOR_BITNESS_565,
                           rgb565_big_endian_,
                           x_off, y_off, stride - x_off - w);
  ft_draw_us_ += (uint32_t)(esp_timer_get_time() - t0);
}

bool RemoteWebView::ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid) {
  if (touch_disabled_)
    return false;

  x = std::clamp(x, 0, 65535);
  y = std::clamp(y, 0, 65535);

  uint8_t pkt[sizeof(proto::TouchPacket)];
  const size_t n = proto::build_touch_packet(type, pid, x, y, pkt);

  const bool ok = ws_send_bin_(pkt, n, pdMS_TO_TICKS(10), pdMS_TO_TICKS(50));
  if (ok && type == proto::TouchType::Up && stats_interval_ms_)
    touch_up_us_.store(esp_timer_get_time());
  return ok;
}

bool RemoteWebView::ws_send_open_url_(const char *url, uint16_t flags) {
  if (!url || !connected_client_())
    return false;

  const uint32_t n = (uint32_t) strlen(url);
  const size_t total = sizeof(proto::OpenURLHeader) + (size_t) n;
  
  if (total > 16 * 1024) return false;

    auto *pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  if (!pkt) return false;

  const size_t written = proto::build_open_url_packet(url, flags, pkt, total);
  const bool ok = written && ws_send_bin_(pkt, written, pdMS_TO_TICKS(50), pdMS_TO_TICKS(200));
  free(pkt);
  return ok;
}

bool RemoteWebView::ws_send_keepalive_() {
  uint8_t pkt[sizeof(proto::KeepalivePacket)];
  const size_t n = proto::build_keepalive_packet(pkt);
  return n && ws_send_bin_(pkt, n, pdMS_TO_TICKS(50), pdMS_TO_TICKS(50));
}

bool RemoteWebView::ws_send_request_full_frame_() {
  uint8_t pkt[sizeof(proto::RequestFullFramePacket)];
  const size_t n = proto::build_request_full_frame_packet(pkt);
  return n && ws_send_bin_(pkt, n, pdMS_TO_TICKS(50), pdMS_TO_TICKS(50));
}

esp_websocket_client_handle_t RemoteWebView::connected_client_() const {
  esp_websocket_client_handle_t client = ws_client_.load();
  return (client && esp_websocket_client_is_connected(client)) ? client : nullptr;
}

bool RemoteWebView::ws_send_bin_(const uint8_t *data, size_t len, TickType_t lock_timeout, TickType_t send_timeout) {
  // Load the handle once: ws_client_ is cleared by the WS task on disconnect.
  esp_websocket_client_handle_t client = connected_client_();
  if (!client || !ws_send_mtx_)
    return false;

  if (xSemaphoreTake(ws_send_mtx_, lock_timeout) != pdTRUE)
    return false;

  const int r = esp_websocket_client_send_bin(client, (const char *)data, (int)len, send_timeout);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)len;
}

// url_ is written from the main loop (open_url) and read by the WS task on (re)connect.
// Without the mutex (before setup or if it failed) no tasks are running, so direct access is safe.
std::string RemoteWebView::get_url_() const {
  if (!state_mtx_) return url_;
  xSemaphoreTake(state_mtx_, portMAX_DELAY);
  std::string url = url_;
  xSemaphoreGive(state_mtx_);
  return url;
}

void RemoteWebView::set_url_(const std::string &url) {
  if (!state_mtx_) { url_ = url; return; }
  xSemaphoreTake(state_mtx_, portMAX_DELAY);
  url_ = url;
  xSemaphoreGive(state_mtx_);
}

// ESPHome touch states are bit flags: a point reported by the latest scan is PRESSED or UPDATED,
// a point missing from it gets RELEASING or'ed in and then becomes RELEASED (0).
// STATE_CALIBRATE (all bits set) means uncalibrated coordinates and is treated as not down.
static inline bool touch_point_is_down(const touchscreen::TouchPoint &p) {
  return (p.state & (touchscreen::STATE_PRESSED | touchscreen::STATE_UPDATED)) != 0 &&
         (p.state & touchscreen::STATE_RELEASING) == 0;
}

// The server handles a single touch point, so only one (primary) pointer is forwarded.
// ESPHome calls update() before touch()/release() for the same scan, so update() drives the
// gesture and touch()/release() only cover what it missed. Each transition is sent once.
void RemoteWebViewTouchListener::update(const touchscreen::TouchPoints_t &pts) {
  if (!parent_) return;

  if (active_) {
    for (auto &p : pts) {
      if (p.id == active_id_ && touch_point_is_down(p)) {
        move_to_(p.x, p.y);
        return;
      }
    }
    end_();
    // Other fingers may still be down; don't turn them into a new gesture.
    for (auto &p : pts) {
      if (touch_point_is_down(p)) { wait_all_up_ = true; break; }
    }
    return;
  }

  bool any_down = false;
  for (auto &p : pts) {
    if (!touch_point_is_down(p)) continue;
    any_down = true;
    if (!wait_all_up_) {
      begin_(p);
      return;
    }
  }
  if (!any_down) wait_all_up_ = false;
}

void RemoteWebViewTouchListener::touch(touchscreen::TouchPoint tp) {
  if (!parent_ || active_ || wait_all_up_) return;
  begin_(tp);
}

void RemoteWebViewTouchListener::release() {
  if (!parent_) return;
  if (active_) end_();
  wait_all_up_ = false;
}

void RemoteWebViewTouchListener::begin_(const touchscreen::TouchPoint &p) {
  if (!parent_->ws_send_touch_event_(proto::TouchType::Down, p.x, p.y, p.id)) {
    // Not sent (disconnected or touch disabled): ignore the rest of this gesture.
    wait_all_up_ = true;
    return;
  }
  active_ = true;
  active_id_ = p.id;
  last_x_ = p.x;
  last_y_ = p.y;
  last_move_us_ = esp_timer_get_time();
  move_pending_ = false;
}

void RemoteWebViewTouchListener::move_to_(int x, int y) {
  if (x == last_x_ && y == last_y_ && !move_pending_) return;
  last_x_ = x;
  last_y_ = y;

  const uint64_t now = esp_timer_get_time();
  if (RemoteWebView::kCoalesceMoves && RemoteWebView::kMoveIntervalUs != 0 &&
      (now - last_move_us_) < RemoteWebView::kMoveIntervalUs) {
    move_pending_ = true;  // sent with a later update or right before Up
    return;
  }
  last_move_us_ = now;
  move_pending_ = false;
  parent_->ws_send_touch_event_(proto::TouchType::Move, x, y, active_id_);
}

void RemoteWebViewTouchListener::end_() {
  if (move_pending_)
    parent_->ws_send_touch_event_(proto::TouchType::Move, last_x_, last_y_, active_id_);
  parent_->ws_send_touch_event_(proto::TouchType::Up, last_x_, last_y_, active_id_);
  active_ = false;
  move_pending_ = false;
}

void RemoteWebView::disable_touch(bool disable) {
  touch_disabled_ = disable;
  ESP_LOGD(TAG, "touch %s", disable ? "disabled" : "enabled");
}

void RemoteWebView::set_server(const std::string &s) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos || pos == s.size() - 1) {
    ESP_LOGE(TAG, "server must be host:port, got: %s", s.c_str());
    return;
  }
  server_host_ = s.substr(0, pos);
  server_port_ = atoi(s.c_str() + pos + 1);
  if (server_port_ <= 0 || server_port_ > 65535) {
    ESP_LOGE(TAG, "invalid port in server: %s", s.c_str());
    server_host_.clear();
    server_port_ = 0;
  }
}

void RemoteWebView::append_q_int_(std::string &s, const char *k, int v) {
  if (v < 0) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  snprintf(buf, sizeof(buf), "%s=%d", k, v);
  s += buf;
}

void RemoteWebView::append_q_float_(std::string &s, const char *k, float v) {
  if (v < 0.0f) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  char buf[32];
  
  snprintf(buf, sizeof(buf), "%s=%.2f", k, (double)v);
  s += buf;
}

void RemoteWebView::append_q_str_(std::string &s, const char *k, const char *v) {
  if (!v || !*v) return;
  s += (s.find('?') == std::string::npos) ? '?' : '&';
  s += k; s += '=';

  // Percent-encode everything except RFC 3986 unreserved characters.
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
    const unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      s += (char)c;
    } else {
      s += '%';
      s += hex[c >> 4];
      s += hex[c & 0x0F];
    }
  }
}

std::string RemoteWebView::resolve_device_id_() const {
  if (!device_id_.empty()) return device_id_;

  uint8_t mac[6] = {0};
  esp_err_t err = ESP_FAIL;
  
#if ESP_IDF_VERSION_MAJOR >= 5
  err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_BT);
  }
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_ETH);
  }
  if (err != ESP_OK) {
    err = esp_efuse_mac_get_default(mac);
  }
#else
  err = esp_efuse_mac_get_default(mac);
  if (err != ESP_OK) {
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
#endif

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read MAC address, using random ID");
    esp_fill_random(mac, sizeof(mac));
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

std::string RemoteWebView::build_ws_uri_() const {
  std::string uri;
  uri = "ws://" + server_host_ + ":" + std::to_string(server_port_);
  uri += "/";

  const std::string id = resolve_device_id_();
  append_q_str_(uri, "id", id.c_str());

  append_q_int_(uri, "w", display_width_);
  append_q_int_(uri, "h", display_height_);

  append_q_int_(uri,   "r",    rotation_);
  append_q_int_(uri,   "ts",   tile_size_);
  append_q_int_(uri,   "fftc", full_frame_tile_count_);
  append_q_float_(uri, "ffat", full_frame_area_threshold_);
  append_q_int_(uri,   "ffe",  full_frame_every_);
  append_q_int_(uri,   "enf",  every_nth_frame_);
  append_q_int_(uri,   "mfi",  min_frame_interval_);
  append_q_int_(uri,   "q",    jpeg_quality_);
  append_q_int_(uri,   "mbpm", max_bytes_per_msg_);

  return uri;
}

}  // namespace remote_webview
}  // namespace esphome
