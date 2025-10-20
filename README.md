# Remote WebView Client

![Guition-ESP32-S3-4848S040 running Remote WebView](/images/image-001.jpg)

[Demo video](https://youtu.be/rD2aYUUrv5o)

Here you can find clients for [Remote WebView Server](https://github.com/strange-v/RemoteWebViewServer) — a headless browser that renders target web pages (e.g., Home Assistant dashboards) and streams them as image tiles over WebSocket to lightweight clients (ESP32 displays).

## ESPHome component

The latest version of the client is implemented as an ESPHome external component, which greatly simplifies installation and configuration for end users. It leverages the display and touchscreen components to render images and handle touch input.

### Configuration example (Guition-ESP32-S3-4848S040)

- Add your credentials to secrets.yaml
![ESPHome Builder](/images/image-002.jpg)
- Change the substitutions according to your needs.

```yaml
substitutions:
  name: esp32-4848s040          # Name of the display
  number: 1                     # Consecutive number of the display
  friendly_name: ESP32-Display Kitchen 
  haip: homeassistant           # Your Home Assistant url or IP
  starturl: https://github.com  # Set url: "self-test" to initiate the self-test
  delay: 5min                   # Time till displaybacklight turns off. Possible: s/min/h
                                # If you want the display to stay always on, use the alwayson.yaml example
esphome:
  name: ${name}-${number}
  friendly_name:  ${friendly_name}
  platformio_options:
    board_build.flash_mode: dio
    board_build.flash_mode: dio

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  flash_size: 16MB
  framework:
    type: esp-idf
    sdkconfig_options:
      COMPILER_OPTIMIZATION_SIZE: y
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: "y"
      CONFIG_ESP32S3_DATA_CACHE_64KB: "y"
      CONFIG_ESP32S3_DATA_CACHE_LINE_64B: "y"
      CONFIG_SPIRAM_FETCH_INSTRUCTIONS: y
      CONFIG_SPIRAM_RODATA: y
    components:
      - name: "espressif/esp_websocket_client"
        ref: 1.5.0
      - name: "espressif/esp-dsp"
        ref: 1.7.0
      - name: "bitbank2/jpegdec"
        source: https://github.com/strange-v/jpegdec-esphome

psram:
  mode: octal
  speed: 80MHz

external_components:
  - source: github://strange-v/RemoteWebViewClient@main
    refresh: 0s
    components: [ remote_webview ]

logger:
  hardware_uart: UART0

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Display Fallback Hotspot"
    password: ""

captive_portal:
    
spi:
  clk_pin: GPIO48
  mosi_pin: GPIO47

i2c:
  - id: bus_a
    sda: GPIO19
    scl: GPIO45

display:
  - platform: st7701s
    id: espdisplay_${number}
    show_test_card: False
    update_interval: never
    auto_clear_enabled: False
    spi_mode: MODE3
    data_rate: 2MHz
    color_order: RGB
    invert_colors: False
    dimensions:
      width: 480
      height: 480
    cs_pin: 39
    de_pin: 18
    hsync_pin: 16
    vsync_pin: 17
    pclk_pin: 21
    pclk_frequency: 12MHz
    pclk_inverted: False
    hsync_pulse_width: 8
    hsync_front_porch: 10
    hsync_back_porch: 20
    vsync_pulse_width: 8
    vsync_front_porch: 10
    vsync_back_porch: 10
    init_sequence:
      - 1
      - [0xFF, 0x77, 0x01, 0x00, 0x00, 0x10]
      - [0xCD, 0x00]
    data_pins:
      red:
        - GPIO11
        - GPIO12
        - GPIO13
        - GPIO14
        - GPIO0
      green:
        - GPIO8
        - GPIO20
        - GPIO3
        - GPIO46
        - GPIO9
        - GPIO10
      blue:
        - GPIO4
        - GPIO5
        - GPIO6
        - GPIO7
        - GPIO15

touchscreen:
  platform: gt911
  transform:
    mirror_x: false
    mirror_y: false
  i2c_id: bus_a
  id: esptouchscreen_${number}
  display: espdisplay_${number}
  on_touch:
    then:
      - light.turn_on:
          id: back_light
          brightness: 1.0

script:
  - id: reset_backlight_timer
    mode: restart
    then:
      - delay: ${delay}
      - light.turn_off: back_light

output:
  - platform: ledc
    pin: GPIO38
    id: backlight_pwm

light:
  - platform: monochromatic
    output: backlight_pwm
    name: "Display Backlight"
    id: back_light
    restore_mode: ALWAYS_ON
    on_turn_on:
      then:
        - script.execute: reset_backlight_timer

remote_webview:
  id: rwv
  display_id: espdisplay_${number}
  touchscreen_id: esptouchscreen_${number}
  device_id: ${name}
  server: ${haip}:8081
  url: ${starturl}
  full_frame_tile_count: 1
  max_bytes_per_msg: 61440
  jpeg_quality: 85

text:
  - platform: template
    id: rwv_url
    name: "URL"
    optimistic: true
    restore_value: false
    mode: TEXT
    min_length: 1
    set_action:
      - lambda: |-
          if (!id(rwv).open_url(std::string(x.c_str()))) {
            id(rwv).set_url(std::string(x.c_str()));
            ESP_LOGI("remote_webview", "URL queued (not connected): %s", x.c_str());
          }

```

### Variables to change 
| YAML variable    | Example            | Description  |
|------------------|--------------------|--------------|
| name             | esp32-4848s040     | Name of the display |
| number           | 1                  | Consecutive number of the display |
| friendly_name    | ESP32-Display Kitchen | Name in HA |
| haip             | homeassistant       | Your Home Assistant url or IP |
| starturl         | https://github.com  | Set the "Homepage" of the display. Change according your needs. It wcould be something like: http://homeassistant:8123/config/dashboard |
| delay            | 5min                | Time till displaybacklight turns off. Possible: s/min/h |

### Supported Parameters
| YAML key                | Type      | Required | Example                          | Description |
|-------------------------|-----------|:--------:|----------------------------------|-------------|
| `display_id`            | id        | ✅       | `panel`                           | Display to draw on. |
| `touchscreen_id`        | id        | ❌       | `touch`                           | Optional touch input source. When present, touch events (down/move/up) are sent to the server. |
| `server`                | string    | ✅       | `172.16.0.252:8081`              | WebSocket server address. Must be `hostname_or_IP:port`. |
| `url`                   | string    | ✅       | `http://…/dashboard`             | Page to open on connect. |
| `device_id`             | string    | ❌       | `"my-device"` or auto (`esp32-<mac>`) | Identifier used by the server. If not set, the component derives `esp32-<mac>` from the chip MAC and still sends it. |
| `tile_size`             | int       | ❌       | `32`                              | Tile edge size in pixels. Helps the server choose tile packing; it’s best to keep it a multiple of 16. |
| `full_frame_tile_count` | int       | ❌       | `4`                               | Number of tiles the server should use for full-frame updates. |
| `full_frame_area_threshold` | float | ❌       | `0.50`                            | Area delta (fraction of screen) above which the server should send a full frame. |
| `full_frame_every`      | int       | ❌       | `50`                              | Force a full-frame update every N frames (0 disables). |
| `every_nth_frame`       | int       | ❌       | `1`                               | Frame-rate divider. Server should send only every Nth frame. |
| `min_frame_interval`    | int (ms)  | ❌       | `80`                              | Minimum time between frames on the wire, in milliseconds. |
| `jpeg_quality`          | int       | ❌       | `85`                              | JPEG quality hint for the server’s encoder. |
| `max_bytes_per_msg`     | int (B)   | ❌       | `14336` or `61440`                | Upper bound for a single WS binary message. |
| `big_endian`            | bool      | ❌       | `true` or `false`                 | Use big-endian RGB565 pixel order for JPEG output (set false for little-endian panels). |

## Recommendations
- **full_frame_tile_count** set to 1 is the most efficient way to do a full-screen update; use it if your network/device memory allows it.
- **every_nth_frame** must be 1 if you don’t want to miss changes (though increasing it may reduce server load). I recommend keeping it set to 1.
- **min_frame_interval** should be slightly larger than the render time reported by the self-test (set `self-test` as a url parameter in the YAML).
- **max_bytes_per_msg** should be larger than your maximum tile size (full-frame or partial).
- **jpeg_quality** — lower values encode faster and reduce bandwidth (but increase artifacts). Start at **85**, drop toward **70–75** if you need speed.
- **big_endian** — defaults to **true**. If colors look wrong (swapped/tinted), set `big_endian: false` for panels that require little-endian RGB565.
- **Red tile / red screen** — this indicates a tile payload exceeded `max_bytes_per_msg`. Increase `max_bytes_per_msg` or reduce tile size/JPEG quality so each tile fits.

## Standalone clients

Arduino and ESP-IDF clients are deprecated for now. They were used during the PoC/development phase, but I don’t like the idea of supporting multiple codebases for the same project (not everything can be reused as-is due to different ESP-IDF versions, tooling, etc.). However, if demand for a standalone client is high, I may release one.
