#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <LGFX.hpp>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <JPEGDEC.h>

// ---------- Config ----------
#ifndef WIFI_SSID
  #define WIFI_SSID "YOUR_SSID"
#endif
#ifndef WIFI_PWD
  #define WIFI_PWD  "YOUR_PASS"
#endif
#ifndef WS_HOST
  #define WS_HOST   "172.16.0.252"
#endif
#ifndef WS_PORT
  #define WS_PORT   8081
#endif
#ifndef DEVICE_ID
  #define DEVICE_ID "master-bedroom"
#endif
#ifndef FB_W
  #define FB_W 800
#endif
#ifndef FB_H
  #define FB_H 480
#endif

// ---------- Derived ----------
static String WS_PATH = String("/?id=") + DEVICE_ID;

// ---------- Peripherals ----------
static LGFX lcd;
static WebSocketsClient ws;
static JPEGDEC jdec;

// ---------- Protocol ----------
static constexpr uint8_t  FRAM_VER = 1;
enum Encoding : uint8_t {
  ENC_UNKNOWN    = 0,
  ENC_PNG        = 1, // not implemented
  ENC_JPEG       = 2,
  ENC_RAW565     = 3, // not implemented
  ENC_RAW565_RLE = 4, // not implemented
  ENC_RAW565_LZ4 = 5  // not implemented
};
static constexpr uint16_t FLAG_LAST_OF_FRAME = 1 << 0;
static constexpr uint16_t FLAG_IS_FULL_FRAME = 1 << 1;

// ---------- Stats ----------
static uint32_t g_currFrameId = 0xFFFFFFFF;
static uint32_t g_frameStartMs = 0;
static uint16_t g_tilesSeen = 0;
static size_t   g_bytesThisFrame = 0;

// FPST stats: average total frame render time (ms)
static uint64_t g_msSum = 0;
static uint32_t g_msCnt = 0;

// ---------- Helpers ----------
static inline uint16_t rd16LE(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32LE(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Use DMA for sufficiently large blocks to reduce CPU copy cost.
static inline bool useDMA(int w, int h) {
  return (w * h) >= 512; // tiny heuristic; safe because we waitDMA() right after
}

// ---------- JPEG → LCD (direct) ----------
static int jpegDrawToLCD(JPEGDRAW *pDraw) {
  int x = pDraw->x;
  int y = pDraw->y;
  int w = pDraw->iWidth;
  int h = pDraw->iHeight;

  if (x >= FB_W || y >= FB_H) return 1;
  if (x + w > FB_W) w = FB_W - x;
  if (y + h > FB_H) h = FB_H - y;
  if (w <= 0 || h <= 0) return 1;

  const uint16_t* pix = (const uint16_t*)pDraw->pPixels; // JPEGDEC gives RGB565 (byte order handled by setSwapBytes)

  if (useDMA(w, h)) {
    lcd.pushImageDMA(x, y, w, h, pix);
    lcd.waitDMA();       // JPEGDEC may reuse its buffer; ensure DMA finished
  } else {
    lcd.pushImage(x, y, w, h, pix);
  }
  return 1;
}

static bool decodeJpegToLCD(int16_t x, int16_t y, const uint8_t *data, size_t len) {
  if (!data || len == 0) return false;
  int rc = jdec.openRAM((uint8_t*)data, (int)len, jpegDrawToLCD);
  if (rc != 1) return false;
  lcd.startWrite();            // batch multiple pushImage* within one tile
  rc = jdec.decode(x, y, 0);
  lcd.endWrite();
  jdec.close();
  return (rc == 1);
}

static void wsSendFPST() {
  uint32_t avg = (g_msCnt ? (uint32_t)(g_msSum / g_msCnt) : 0);
  Serial.printf("FPST: %u ms\n", (unsigned)avg);
  uint8_t buf[8];
  buf[0]='F'; buf[1]='P'; buf[2]='S'; buf[3]='T';
  buf[4] = (uint8_t)(avg & 0xFF);
  buf[5] = (uint8_t)((avg >> 8) & 0xFF);
  buf[6] = (uint8_t)((avg >> 16) & 0xFF);
  buf[7] = (uint8_t)((avg >> 24) & 0xFF);
  ws.sendBIN(buf, sizeof(buf));
  g_msSum = 0;
  g_msCnt = 0;
}

static void processFramPacket(const uint8_t *data, size_t len) {
  if (len < (4 + 1 + 4 + 1 + 2 + 2)) return;
  if (memcmp(data, "FRAM", 4) != 0) return;

  size_t off = 4;
  const uint8_t ver = data[off++]; if (ver != FRAM_VER) return;
  const uint32_t frame_id = rd32LE(data + off); off += 4;
  const uint8_t  enc      = data[off++];       // only JPEG
  const uint16_t count    = rd16LE(data + off); off += 2;
  const uint16_t flags    = rd16LE(data + off); off += 2;

  if (frame_id != g_currFrameId) {
    g_currFrameId = frame_id;
    g_frameStartMs = millis();
    g_tilesSeen = 0;
    g_bytesThisFrame = 0;
  }

  g_bytesThisFrame += len;

  for (uint16_t i = 0; i < count; i++) {
    if (off + 2+2+2+2+4 > len) return;
    const uint16_t x    = rd16LE(data + off); off += 2;
    const uint16_t y    = rd16LE(data + off); off += 2;
    const uint16_t w    = rd16LE(data + off); off += 2;
    const uint16_t h    = rd16LE(data + off); off += 2;
    const uint32_t dlen = rd32LE(data + off); off += 4;
    if (off + dlen > len) return;

    if (enc == ENC_JPEG && dlen) {
      (void)decodeJpegToLCD((int16_t)x, (int16_t)y, (const uint8_t*)(data + off), (size_t)dlen);
    }
    off += dlen;
    g_tilesSeen++;
    ws.loop();
    yield();
  }

  if (flags & FLAG_LAST_OF_FRAME) {
    const uint32_t frame_ms = millis() - g_frameStartMs;
    g_msSum += frame_ms;
    g_msCnt += 1;

    Serial.printf("frame %lu tiles %u (%u bytes): %lu ms\n",
                  (unsigned long)g_currFrameId,
                  (unsigned)g_tilesSeen,
                  (unsigned)g_bytesThisFrame,
                  (unsigned long)frame_ms);
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_BIN:
      if (length >= 4 && memcmp(payload, "FPST", 4) == 0) { wsSendFPST(); break; }
      processFramPacket(payload, length);
      break;
    case WStype_TEXT:
      if (length >= 4 && memcmp(payload, "FPST", 4) == 0) wsSendFPST();
      break;
    case WStype_CONNECTED:    Serial.println("[ws] connected");   break;
    case WStype_DISCONNECTED: Serial.println("[ws] disconnected");break;
    default: break;
  }
}

static void wsSendTouchBin(uint8_t kind, uint16_t x, uint16_t y) {
  uint8_t f[9] = {'T','O','U','C', kind, (uint8_t)(x&0xFF),(uint8_t)(x>>8),(uint8_t)(y&0xFF),(uint8_t)(y>>8)};
  ws.sendBIN(f, sizeof(f));
}
static void wsSendTouchIfNeeded() {
  static bool wasDown = false; static int16_t px=-1, py=-1;
  uint16_t x,y; bool isDown = lcd.getTouch(&x,&y);
  if (isDown && !wasDown) { wsSendTouchBin(0,x,y); px=x; py=y; }
  else if (isDown && wasDown) {
    if (abs((int)x-(int)px) + abs((int)y-(int)py) >= 3) { wsSendTouchBin(1,x,y); px=x; py=y; }
  } else if (!isDown && wasDown) { wsSendTouchBin(2,px,py); }
  wasDown = isDown;
}

static void connectWiFi() {
  Serial.printf("WiFi connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) { Serial.print('.'); delay(400); if (millis()-t0>20000) break; }
  Serial.printf("\nWiFi %s, ip: %s\n",
    WiFi.status()==WL_CONNECTED ? "OK":"FAIL",
    WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);

  lcd.init();
  lcd.setRotation(0);
  lcd.setSwapBytes(true);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("Connecting WiFi...", lcd.width()/2, lcd.height()/2);

  connectWiFi();

  lcd.fillScreen(TFT_BLACK);
  lcd.drawString("Connecting WS...", lcd.width()/2, lcd.height()/2);

  ws.begin(WS_HOST, WS_PORT, WS_PATH.c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(2000);
  ws.enableHeartbeat(15000, 3000, 2);

  lcd.setBrightness(150);
}

void loop() {
  ws.loop();
  static uint32_t lastTouch=0; uint32_t now=millis();
  if (now - lastTouch >= 15) { wsSendTouchIfNeeded(); lastTouch = now; }
}
