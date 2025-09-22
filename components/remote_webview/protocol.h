#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace esphome::remote_webview::proto {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kFlafLastOfFrame = 1u<<0;
constexpr uint8_t kFlagIsFullFrame = 1u<<1;

enum class MsgType   : uint8_t { Unknown = 0, Frame = 1, Touch = 2, FrameStats = 3, OpenURL = 4, Keepalive = 5 };
enum class Encoding  : uint8_t { Unknown = 0, PNG = 1, JPEG = 2, RAW565 = 3, RAW565_RLE = 4, RAW565_LZ4 = 5 };
enum class TouchType : uint8_t { Unknown = 0, Down = 1, Move = 2, Up = 3 };

#if defined(__GNUC__)
  #define RWV_PACKED __attribute__((packed))
#else
  #pragma pack(push, 1)
  #define RWV_PACKED
#endif

// [type:1][ver:1][frame_id:4][enc:1][tile_count:2][flags:2]  => 11 bytes
struct RWV_PACKED FrameHeader {
  MsgType type;
  uint8_t version;
  uint32_t frame_id;
  Encoding encoding;
  uint16_t tile_count;
  uint16_t flags;
};
static_assert(sizeof(FrameHeader) == 11, "FrameHeader wire size must be 11");

// [x:2][y:2][w:2][h:2][dlen:4] => 12 bytes
struct RWV_PACKED TileHeader {
  uint16_t x, y, w, h;
  uint32_t dlen;
};
static_assert(sizeof(TileHeader) == 12, "TileHeader wire size must be 12");

// [type:1][ver:1][subtype:1][pointer_id:1][x:2][y:2] => 8 bytes
struct RWV_PACKED TouchPacket {
  MsgType type;
  uint8_t version;
  TouchType subtype;
  uint8_t pointer_id;
  uint16_t x;
  uint16_t y;
};
static_assert(sizeof(TouchPacket) == 8, "TouchPacket wire size must be 8");

// [type:1][ver:1][flags:2][url_len:4] => 8 bytes
struct RWV_PACKED OpenURLHeader {
  MsgType type;
  uint8_t ver;
  uint16_t flags;
  uint32_t url_len;
};
static_assert(sizeof(OpenURLHeader) == 8, "OpenURLHeader wire size must be 8");

// [type:1][ver:1][time:4][count:4] => 10 bytes
struct RWV_PACKED FrameStatsPacket {
  MsgType type;
  uint8_t ver;
  uint32_t avg_time;
  uint32_t bytes;
};
static_assert(sizeof(FrameStatsPacket) == 10, "FrameStatsPacket wire size must be 10");

#if !defined(__GNUC__)
  #pragma pack(pop)
#endif
#undef RWV_PACKED

inline uint16_t rd16(const uint8_t *p){ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

struct FrameInfo {
  uint32_t frame_id;
  Encoding enc;
  uint16_t tile_count;
  uint16_t flags;
};

inline bool parse_frame_header(const uint8_t *data, size_t len, FrameInfo &out, size_t &off) {
  if (len < sizeof(FrameHeader)) return false;

  FrameHeader h;
  memcpy(&h, data, sizeof(h));

  if (h.type != MsgType::Frame || h.version != kProtocolVersion) return false;

  out.frame_id = rd32(reinterpret_cast<const uint8_t*>(&h.frame_id));
  out.enc = h.encoding;
  out.tile_count = rd16(reinterpret_cast<const uint8_t*>(&h.tile_count));
  out.flags = rd16(reinterpret_cast<const uint8_t*>(&h.flags));
  off = sizeof(FrameHeader);
  
  return true;
}

inline bool parse_tile_header(const uint8_t *buf, size_t len, TileHeader &out, size_t &off) {
  if (!buf) return false;
  if (off + sizeof(TileHeader) > len) return false;

  const uint8_t *th = buf + off;
  out.x       = rd16(th + 0);
  out.y       = rd16(th + 2);
  out.w       = rd16(th + 4);
  out.h       = rd16(th + 6);
  out.dlen    = rd32(th + 8);

  off += sizeof(TileHeader);
  return true;
}

inline size_t build_touch_packet(TouchType t, uint8_t pid, uint16_t x, uint16_t y, uint8_t *out) {
  TouchPacket pkt{};
  pkt.type = MsgType::Touch;
  pkt.version = kProtocolVersion;
  pkt.subtype = t;
  pkt.pointer_id = pid;
  
  uint8_t *px = reinterpret_cast<uint8_t*>(&pkt.x);
  uint8_t *py = reinterpret_cast<uint8_t*>(&pkt.y);
  wr16(px, x);
  wr16(py, y);
  memcpy(out, &pkt, sizeof(pkt));
  
  return sizeof(pkt);
}

inline size_t build_open_url_packet(const char *url, uint16_t flags, uint8_t *out, size_t out_cap) {
  if (!url || !out) return 0;

  const uint32_t n = (uint32_t) strlen(url);
  const size_t total = sizeof(OpenURLHeader) + (size_t) n;
  if (total > out_cap) return 0;

  auto *p = reinterpret_cast<OpenURLHeader *>(out);
  p->type    = MsgType::OpenURL;
  p->ver     = kProtocolVersion;
  p->flags   = flags;
  p->url_len = n;

  memcpy(out + sizeof(OpenURLHeader), url, n);
  return total;
}

inline size_t build_frame_stats_packet(uint32_t avg_time, uint32_t bytes, uint8_t *out) {
  if (!out) return 0;

  FrameStatsPacket pkt{};
  pkt.type = MsgType::FrameStats;
  pkt.ver = kProtocolVersion;
  pkt.avg_time = avg_time;
  pkt.bytes = bytes;

  memcpy(out, &pkt, sizeof(pkt));
  return sizeof(pkt);
}

} // namespace esphome::remote_webview::proto
