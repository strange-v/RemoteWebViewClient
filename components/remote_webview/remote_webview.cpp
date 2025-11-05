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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace remote_webview {

static const char *const TAG = "Remove_WebView";
RemoteWebView *RemoteWebView::self_ = nullptr;

void RemoteWebView::setup() {
  self_ = this;

  if (!display_) {
    ESP_LOGE(TAG, "no display");
    return;
  }

  q_decode_    = xQueueCreate(cfg::decode_queue_depth, sizeof(WsMsg));
  ws_send_mtx_ = xSemaphoreCreateMutex();

  start_decode_task_();
  start_ws_task_();

  if (touch_) {
    touch_listener_ = new RemoteWebViewTouchListener(this);
    touch_->register_listener(touch_listener_);
    ESP_LOGI(TAG, "touch listener registered");
  }

#if REMOTE_WEBVIEW_HW_JPEG
  jpeg_decode_engine_cfg_t jcfg = {
    .timeout_ms = 200,
  };
  if (jpeg_new_decoder_engine(&jcfg, &hw_dec_) != ESP_OK) {
    hw_dec_ = nullptr;
  }
  
  if (hw_dec_) {
    const int W = display_->get_width();
    const int H = display_->get_height();
    const int aligned_w = (W + 15) & ~15;
    const int aligned_h = (H + 15) & ~15;
    
    // Allocate same size for input and output - JPEG can't be larger than uncompressed RGB565
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
      ESP_LOGI(TAG, "HW decoder buffers allocated: input=%u, output=%u", 
               (unsigned)hw_decode_input_size_, (unsigned)hw_decode_output_size_);
    }
  }
#endif
}

void RemoteWebView::dump_config() {
  ESP_LOGCONFIG(TAG, "remote_webview:");

  const std::string id = device_id_.empty() ? resolve_device_id_() : device_id_;
  ESP_LOGCONFIG(TAG, "  id: %s", id.c_str());

  if (display_) {
    ESP_LOGCONFIG(TAG, "  display: %dx%d", display_->get_width(), display_->get_height());
  }

  ESP_LOGCONFIG(TAG, "  server: %s:%d", server_host_.c_str(), server_port_);
  ESP_LOGCONFIG(TAG, "  url: %s", url_.c_str());

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
  
  if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_))
    return false;
  
  if (ws_send_open_url_(s.c_str(), 0)) {
    url_ = s;
    ESP_LOGI(TAG, "opened URL: %s", s.c_str());
    return true;
  }
  
  return false;
}

void RemoteWebView::start_ws_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::ws_task_tramp_, "rwv_ws", cfg::ws_task_stack, this, 5, &t_ws_, 0);
}

void RemoteWebView::ws_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);

  std::string uri_str = self->build_ws_uri_();
  esp_websocket_client_config_t cfg_ws = {};
  cfg_ws.uri = uri_str.c_str();
  cfg_ws.reconnect_timeout_ms = 2000;
  cfg_ws.network_timeout_ms   = 10000;
  cfg_ws.task_stack           = cfg::ws_task_stack;
  cfg_ws.task_prio            = cfg::ws_task_prio;
  cfg_ws.buffer_size          = cfg::ws_buffer_size;
  cfg_ws.disable_auto_reconnect = false;

  WsReasm reasm{};
  esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg_ws);
  ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler_, &reasm));
  ESP_ERROR_CHECK(esp_websocket_client_start(client));

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (self && self->ws_client_ && esp_websocket_client_is_connected(self->ws_client_)) {
      const uint64_t now = esp_timer_get_time();
      if (now - self->last_keepalive_us_ >= cfg::ws_keepalive_interval_us) {
        if (self->ws_send_keepalive_()) {
          self->last_keepalive_us_ = now;
          ESP_LOGV(TAG, "[ws] keepalive sent");
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
      if (self_) self_->ws_client_ = e->client;
      ESP_LOGI(TAG, "[ws] connected");
      
      if (self_) self_->last_keepalive_us_ = esp_timer_get_time();
      if (self_ && !self_->url_.empty()) {
        self_->ws_send_open_url_(self_->url_.c_str(), 0);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      if (self_) self_->ws_client_ = nullptr;
      ESP_LOGW(TAG, "[ws] disconnected");
      if (self_) self_->last_keepalive_us_ = 0; 
      reasm_reset_(*r);
      break;

    case WEBSOCKET_EVENT_DATA: {
      if (!self_) break;

      const uint8_t *frag = (const uint8_t *)e->data_ptr;
      size_t frag_len = (size_t)e->data_len;
      bool is_bin  = (e->op_code == WS_TRANSPORT_OPCODES_BINARY);
      if (!is_bin) break;

      if (e->payload_offset == 0) {
        reasm_reset_(*r);
        if ((size_t)e->payload_len > cfg::ws_max_message_bytes) {
          ESP_LOGE(TAG, "WS message too large: %u > %u", (unsigned)e->payload_len, (unsigned)cfg::ws_max_message_bytes);
          break;
        }
        r->total = (size_t)e->payload_len;
        r->buf   = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->buf) r->buf = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_8BIT);
        if (!r->buf) { ESP_LOGE(TAG, "malloc %u failed", (unsigned)r->total); r->total = 0; break; }
      }
      if (!r->buf || r->total == 0) break;

      if ((size_t)e->payload_offset + frag_len > r->total) {
        ESP_LOGE(TAG, "bad fragment bounds");
        reasm_reset_(*r);
        break;
      }
      memcpy(r->buf + e->payload_offset, frag, frag_len);
      size_t new_filled = (size_t)e->payload_offset + frag_len;
      if (new_filled > r->filled) r->filled = new_filled;

      if (r->filled == r->total) {
        WsMsg m;
        m.buf = r->buf; m.len = r->total; m.client = e->client;
        r->buf = nullptr; r->total = 0; r->filled = 0;
        if (!self_->q_decode_ || xQueueSend(self_->q_decode_, &m, 0) != pdTRUE) {
          ESP_LOGW(TAG, "decode queue full, dropping packet");
          free(m.buf);
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

void RemoteWebView::start_decode_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::decode_task_tramp_, "rwv_decode", cfg::decode_task_stack, this, 6, &t_decode_, 1);
}

void RemoteWebView::decode_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView*>(arg);
  WsMsg m;
  for (;;) {
    if (xQueueReceive(self->q_decode_, &m, portMAX_DELAY) == pdTRUE) {
      self->process_packet_(m.client, m.buf, m.len);
      free(m.buf);
    }
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
    default:
      ESP_LOGW(TAG, "unknown packet type: %d", (int)type);
      break;
  }
}

void RemoteWebView::process_frame_packet_(const uint8_t *data, size_t len)
{
  if (!data || len < sizeof(proto::FrameHeader)) return;

  proto::FrameInfo fi{};
  size_t off = 0;
  if (!proto::parse_frame_header(data, len, fi, off)) return;

  if (fi.frame_id != frame_id_) {
    frame_id_ = fi.frame_id;
    frame_tiles_= 0;
    frame_bytes_= 0;
    frame_start_us_ = esp_timer_get_time();
  }
  frame_bytes_ += len;
  frame_tiles_ += fi.tile_count;

  const int FB_W = display_ ? display_->get_width()  : 480;
  const int FB_H = display_ ? display_->get_height() : 480;

  for (uint16_t i = 0; i < fi.tile_count; i++) {
    proto::TileHeader th{};
    if (!proto::parse_tile_header(data, len, th, off)) return;
    if (off + th.dlen > len) return;

    if (th.w == 0 || th.h == 0 || th.w > FB_W || th.h > FB_H) {
      off += th.dlen;
      continue;
    }

    if (fi.enc == proto::Encoding::JPEG && th.dlen) {
      decode_jpeg_tile_to_lcd_((int16_t)th.x, (int16_t)th.y, data + off, th.dlen);
    }
    
    off += th.dlen;
    taskYIELD();
  }

  if (fi.flags & proto::kFlafLastOfFrame) {
    const uint32_t time_ms = (esp_timer_get_time() - frame_start_us_) / 1000ULL;
    frame_stats_bytes_ += frame_bytes_;
    frame_stats_time_ += time_ms;
    frame_stats_count_++;
    ESP_LOGD(TAG, "frame %lu: tiles %u (%u bytes) - %lu ms", frame_id_, frame_tiles_, frame_bytes_, time_ms);
  }
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

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE)
    return;

  esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
}

bool RemoteWebView::decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!display_ || !data || !len) return false;

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
      ESP_LOGW(TAG, "jpeg dimensions not aligned: %u x %u", (unsigned)hdr.width, (unsigned)hdr.height);
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }
    
    if (len > hw_decode_input_size_ || out_sz > hw_decode_output_size_) {
      ESP_LOGW(TAG, "tile too large for HW decoder buffers");
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    jpeg_decode_cfg_t jcfg{};
    jcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    jcfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    jcfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;

    memcpy(hw_decode_input_buf_, data, len);
    
    uint32_t written = 0;
    esp_err_t dr = jpeg_decoder_process(hw_dec_, &jcfg, hw_decode_input_buf_, (uint32_t)len, 
                                        hw_decode_output_buf_, (uint32_t)hw_decode_output_size_, &written);

    if (dr != ESP_OK) {
      return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
    }

    display_->draw_pixels_at(dst_x, dst_y, (int)hdr.width, (int)hdr.height, hw_decode_output_buf_,
        esphome::display::COLOR_ORDER_RGB,
        esphome::display::COLOR_BITNESS_565,
        rgb565_big_endian_);

    return true;
  }
#endif  // REMOTE_WEBVIEW_HW_JPEG

  return decode_jpeg_tile_software_(dst_x, dst_y, data, len);
}

bool RemoteWebView::decode_jpeg_tile_software_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len) {
  if (!jd_.openRAM((uint8_t*)data, (int)len, &RemoteWebView::jpeg_draw_cb_s_)) {
    ESP_LOGW(TAG, "openRAM failed (len=%u) err=%d", (unsigned)len, jd_.getLastError());
    return false;
  }

  jd_.setMaxOutputSize(4 * 2048);
  jd_.setPixelType(rgb565_big_endian_ ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);

  const int rc = jd_.decode(dst_x, dst_y, 0);
  if (rc == 0) {
    ESP_LOGW(TAG, "decode rc=%d err=%d", rc, jd_.getLastError());
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
  if (!display_) return 0;

  int32_t x = p->x, y = p->y, w = p->iWidth, h = p->iHeight;
  const int FB_W = display_->get_width();
  const int FB_H = display_->get_height();

  if (x >= FB_W || y >= FB_H) return 1;
  if (x + w > FB_W) w = FB_W - x;
  if (y + h > FB_H) h = FB_H - y;
  if (w <= 0 || h <= 0) return 1;

  const bool big_endian = rgb565_big_endian_;
  display_->draw_pixels_at(
      x, y, w, h,
      (const uint8_t *)p->pPixels,
      esphome::display::COLOR_ORDER_RGB,
      esphome::display::COLOR_BITNESS_565,
      big_endian
  );

  return 1;
}

bool RemoteWebView::ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid) {
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  // clamp into 16-bit
  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x > 65535) x = 65535; if (y > 65535) y = 65535;

  uint8_t pkt[sizeof(proto::TouchPacket)];
  const size_t n = proto::build_touch_packet(type, pid, x, y, pkt);

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(10)) != pdTRUE)
    return false;

  int r = esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

void RemoteWebViewTouchListener::touch(touchscreen::TouchPoint tp) {
  if (!parent_) return;
  parent_->ws_send_touch_event_(proto::TouchType::Down, tp.x, tp.y, tp.id);
}

bool RemoteWebView::ws_send_open_url_(const char *url, uint16_t flags) {
  if (!ws_client_ || !ws_send_mtx_ ||  !url || !esp_websocket_client_is_connected(ws_client_))
    return false;

  const uint32_t n = (uint32_t) strlen(url);
  const size_t total = sizeof(proto::OpenURLHeader) + (size_t) n;
  if (total > cfg::ws_max_message_bytes) return false;

  // try PSRAM first
  auto *pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *) heap_caps_malloc(total, MALLOC_CAP_8BIT);
  if (!pkt) return false;

  const size_t written = proto::build_open_url_packet(url, flags, pkt, total);
  bool ok = false;
  if (written) {
    if (xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
      const int r = esp_websocket_client_send_bin(ws_client_, (const char *) pkt, (int) written, pdMS_TO_TICKS(200));
      xSemaphoreGive(ws_send_mtx_);
      ok = (r == (int) written);
    }
  }
  free(pkt);
  return ok;
}

bool RemoteWebView::ws_send_keepalive_() {
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_))
    return false;

  uint8_t pkt[sizeof(proto::KeepalivePacket)];
  const size_t n = proto::build_keepalive_packet(pkt);
  if (!n) return false;

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE)
    return false;

  const int r = esp_websocket_client_send_bin(ws_client_, (const char*)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}



void RemoteWebViewTouchListener::update(const touchscreen::TouchPoints_t &pts) {
  if (!parent_) return;
  const uint64_t now = esp_timer_get_time();
  for (auto &p : pts) {
    switch (p.state) {
      case touchscreen::STATE_PRESSED:
        parent_->ws_send_touch_event_(proto::TouchType::Down, p.x, p.y, p.id);
        break;
      case touchscreen::STATE_UPDATED:
        if (!RemoteWebView::kCoalesceMoves || RemoteWebView::kMoveIntervalUs == 0 ||
            (now - parent_->last_move_us_) >= RemoteWebView::kMoveIntervalUs) {
          parent_->last_move_us_ = now;
          parent_->ws_send_touch_event_(proto::TouchType::Move, p.x, p.y, p.id);
        }
        break;
      case touchscreen::STATE_RELEASING:
      case touchscreen::STATE_RELEASED:
        parent_->ws_send_touch_event_(proto::TouchType::Up, p.x, p.y, p.id);
        break;
      default: break;
    }
  }
}

void RemoteWebViewTouchListener::release() {
  if (!parent_) return;
  
  parent_->ws_send_touch_event_(proto::TouchType::Up, 0, 0, 0);
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
  s += k; s += '='; s += v;
}

std::string RemoteWebView::resolve_device_id_() const {
  if (!device_id_.empty()) return device_id_;

  uint8_t mac[6] = {0};
#if ESP_IDF_VERSION_MAJOR >= 5
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    (void) esp_read_mac(mac, ESP_MAC_BT);  // best-effort fallback
  }
#else
  // Older IDF: try to get base MAC from eFuse
  // (some SDKs declare esp_efuse_mac_get_default in esp_system.h)
  extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
  if (esp_efuse_mac_get_default) {
    (void) esp_efuse_mac_get_default(mac);
  } else {
    (void) esp_read_mac(mac, ESP_MAC_WIFI_STA);
  }
#endif

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

  const int W = display_ ? display_->get_width()  : 0;
  const int H = display_ ? display_->get_height() : 0;
  append_q_int_(uri, "w", W);
  append_q_int_(uri, "h", H);

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
