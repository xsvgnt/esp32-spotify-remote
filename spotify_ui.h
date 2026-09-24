#ifndef _SPOTIFY_UI_H_
#define _SPOTIFY_UI_H_

/*
 * spotify_ui.h - LVGL objects, artwork pipeline and net_task.
 *
 * Two tasks, because LVGL is not thread-safe and TLS must never block rendering:
 *   net_task (core 0)       all HTTPS: polls /me/player every 3 s, fetches and
 *                           decodes artwork, executes commands from the UI.
 *   loop()   (core 1)       lv_timer_handler(): all drawing and touch. ui_tick()
 *                           runs every 100 ms, reads the shared state and
 *                           updates the widgets.
 * They share g_ps (PlayerState) behind g_ps_mtx, plus g_cmd_q carrying commands
 * from the UI to the network. No LVGL call ever happens on core 0.
 *
 * Artwork buffers (both 150x150 RGB565 in PSRAM):
 *   g_art_buf    what the lv_img shows. Only written on core 1, by a memcpy in
 *                ui_tick(), so LVGL never renders a half-written cover.
 *   g_stage_buf  decode target of net_task. Holds the prefetched cover of the
 *                next queue item; on a prefetch hit it is memcpy'd into
 *                g_art_buf. While a copy is pending (art_copy_req) net_task
 *                does not touch it.
 */

#include <new>
#include <lvgl.h>
#include <JPEGDEC.h>
#include "spotify_api.h"
#include "scr_st77916.h"
#include "logo_img.h"

// ---------------------------------------------------------------------------
// Build configuration checks (fail early with a readable message)
// ---------------------------------------------------------------------------
#if !defined(LVGL_VERSION_MAJOR) || LVGL_VERSION_MAJOR != 8
#error "LVGL 8.x (8.4.0 or below) is required - 9.x removed lv_disp_drv_t"
#endif
#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 1
#error "lv_conf.h: LV_COLOR_DEPTH must be 16 and LV_COLOR_16_SWAP must be 1"
#endif
#if !LV_TICK_CUSTOM
#error "lv_conf.h: LV_TICK_CUSTOM must be 1 (millis())"
#endif
#if !LV_FONT_MONTSERRAT_14 || !LV_FONT_MONTSERRAT_16 || !LV_FONT_MONTSERRAT_22 || !LV_FONT_MONTSERRAT_32
#error "lv_conf.h: enable LV_FONT_MONTSERRAT_14, _16, _22 and _32"
#endif
#if LV_USE_PERF_MONITOR || LV_USE_MEM_MONITOR
#warning "lv_conf.h: LV_USE_PERF_MONITOR / LV_USE_MEM_MONITOR should be 0"
#endif
#if ARDUINOJSON_VERSION_MAJOR != 7
#error "ArduinoJson 7.x is required"
#endif
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "arduino-esp32 core 3.x is required"
#endif
#ifndef BOARD_HAS_PSRAM
#error "PSRAM is disabled: enable it in Tools > PSRAM"
#endif

// ---------------------------------------------------------------------------
// Layout and timing
// ---------------------------------------------------------------------------
#define ART_SIZE              150
#define ART_BYTES             (ART_SIZE * ART_SIZE * 2)
#define ART_TOP_Y             58
#define ART_CENTER_OFS_Y      (ART_TOP_Y + ART_SIZE / 2 - SCREEN_RES_VER / 2)  // -47 from screen centre
#define ART_MAX_JPEG_BYTES    (512 * 1024)

#define ARC_DIAMETER          336
#define ARC_WIDTH             14
#define TEXT_WIDTH            240     // inside the 254 px safe square
#define TITLE_Y               230
#define ARTIST_Y              262
#define TIME_Y                290
#define NAV_BTN_SIZE          64
#define NAV_BTN_X             118
#define TAP_DISC_SIZE         76

#define COLOR_SPOTIFY_GREEN   0x1DB954
#define COLOR_ARC_TRACK       0x202020
#define COLOR_TEXT_GREY       0xB3B3B3

#define LVGL_DRAW_ROWS        40      // 40 x 360 x 2 = 28.8 KB internal (vendor code uses 72)
#define UI_TICK_MS            100
#define TAP_FEEDBACK_MS       700
#define TOAST_MS              4500
#define IDLE_DIM_AFTER_MS     10000
#define BACKLIGHT_FULL        100
#define BACKLIGHT_DIM         10

#define POLL_INTERVAL_MS      3000
#define POLL_MAX_INTERVAL_MS  30000   // after repeated failures
#define CMD_SETTLE_MS         500     // re-read state this long after a command
#define STAGE_WAIT_MS         1500
#define BAD_URL_MEMORY_MS     600000  // a failed cover URL is not retried for 10 min
#define STATS_EVERY_MS        60000
#define NET_TASK_STACK        16384
#define NET_TASK_CORE         0

// ============================================================================
// Shared state (core 0 <-> core 1)
// ============================================================================
struct PlayerState {
  // idle view: connection status, errors or "Nothing playing"
  char     idle_title[48];
  char     idle_detail[160];
  // current item
  bool     has_item;
  bool     playing;
  char     item_id[64];
  char     title[128];
  char     artist[128];
  char     art_url[256];
  uint32_t duration_ms;
  uint32_t progress_ms;     // playback position at progress_at...
  uint32_t progress_at;     // ...millis() when it was sampled; extrapolated while playing
  uint32_t ui_seq;          // bumped by optimistic UI edits; a poll started earlier must not undo them
  // artwork hand-off
  bool     art_copy_req;    // net -> UI: copy g_stage_buf into g_art_buf
  bool     art_shown;       // g_art_buf holds the current item's cover (else: logo)
  // one-shot message
  char     toast[192];
  uint32_t toast_seq;
};

enum CmdType : uint8_t { CMD_PLAY = 0, CMD_PAUSE, CMD_NEXT, CMD_PREV };
struct Cmd {
  CmdType  type;
  uint32_t issued_at;
};

static PlayerState       g_ps;
static SemaphoreHandle_t g_ps_mtx = nullptr;
static QueueHandle_t     g_cmd_q = nullptr;
static TaskHandle_t      g_net_task = nullptr;

static inline void ps_lock() { xSemaphoreTake(g_ps_mtx, portMAX_DELAY); }
static inline void ps_unlock() { xSemaphoreGive(g_ps_mtx); }

// ============================================================================
// Text helpers
// ============================================================================
// The built-in Montserrat fonts only cover ASCII (+ a few symbols), so fold
// accented Latin letters and typographic punctuation to ASCII: "Beyoncé" is
// shown as "Beyonce" instead of a missing-glyph box. Other scripts pass through.
static const char *const k_fold_latin1[64] = {  // U+00C0 .. U+00FF
  "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
  "D", "N", "O", "O", "O", "O", "O",  "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
  "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
  "d", "n", "o", "o", "o", "o", "o",  "/", "o", "u", "u", "u", "u", "y", "th", "y"};
static const char k_fold_latin_ext_a[] =  // U+0100 .. U+017F, one ASCII letter each
  "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
  "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static void fold_for_font(const char *in, char *out, size_t n) {
  size_t o = 0;
  const uint8_t *p = (const uint8_t *)in;
  while (*p) {
    uint32_t cp;
    size_t len;
    if (p[0] < 0x80) {
      cp = p[0]; len = 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4;
    } else {
      ++p;  // invalid byte: drop it
      continue;
    }
    char one[2] = {0, 0};
    const char *rep = nullptr;
    if (cp >= 0xC0 && cp <= 0xFF) rep = k_fold_latin1[cp - 0xC0];
    else if (cp == 0x152) rep = "OE";
    else if (cp == 0x153) rep = "oe";
    else if (cp >= 0x100 && cp <= 0x17F) { one[0] = k_fold_latin_ext_a[cp - 0x100]; rep = one; }
    else if (cp == 0xA0) rep = " ";
    else if (cp == 0xB4 || cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) rep = "'";
    else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D || cp == 0x201E) rep = "\"";
    else if (cp >= 0x2010 && cp <= 0x2015) rep = "-";
    else if (cp == 0x2026) rep = "...";
    const char *src = rep ? rep : (const char *)p;
    const size_t sl = rep ? strlen(rep) : len;
    if (o + sl >= n) break;  // never cut a UTF-8 sequence in half
    memcpy(out + o, src, sl);
    o += sl;
    p += len;
  }
  out[o] = 0;
}

static void fmt_time(char *out, size_t n, uint32_t ms) {
  const unsigned long s = ms / 1000;
  if (s >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(out, n, "%lu:%02lu", s / 60, s % 60);
}

// ============================================================================
// Shared-state helpers used by net_task
// ============================================================================
// One-shot message shown as a toast on the display (and logged).
static void notify_user(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify_user(const char *fmt, ...) {
  char raw[sizeof(g_ps.toast)], folded[sizeof(g_ps.toast)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(raw, sizeof(raw), fmt, ap);
  va_end(ap);
  fold_for_font(raw, folded, sizeof(folded));
  ps_lock();
  strlcpy(g_ps.toast, folded, sizeof(g_ps.toast));
  g_ps.toast_seq++;
  ps_unlock();
  LOGI("ui", "toast: %s", raw);
}

// Text of the idle view (does not touch the current item).
static void set_idle_text(const char *title, const char *detail) {
  char d[sizeof(g_ps.idle_detail)];
  fold_for_font(detail, d, sizeof(d));
  ps_lock();
  strlcpy(g_ps.idle_title, title, sizeof(g_ps.idle_title));
  strlcpy(g_ps.idle_detail, d, sizeof(g_ps.idle_detail));
  ps_unlock();
}

// ============================================================================
// Artwork: buffers, decoder, download (net_task only, except the buffers)
// ============================================================================
static uint16_t     *g_art_buf = nullptr;    // shown by lv_img, written on core 1 only
static uint16_t     *g_stage_buf = nullptr;  // decode target / prefetch store, written on core 0 only
static lv_img_dsc_t  g_art_dsc;
static JPEGDEC      *g_jpeg = nullptr;       // ~20 KB of decoder state, placed in PSRAM

static char     g_front_url[256];   // cover in (or queued for) g_art_buf
static char     g_stage_url[256];   // cover currently decoded in g_stage_buf
static char     g_art_tried[256];   // URL attempted for the current item (success or failure)
static char     g_bad_url[256];     // last URL that failed
static uint32_t g_bad_at = 0;
static char     g_last_item[64];    // item id the art pipeline last saw
static bool     g_art_pending = false;
static bool     g_prefetch_pending = false;
static uint32_t g_art_hits = 0, g_art_misses = 0;

static bool url_is_bad(const char *url) {
  return g_bad_url[0] && strcmp(url, g_bad_url) == 0 && (millis() - g_bad_at) < BAD_URL_MEMORY_MS;
}
static void mark_bad(const char *url) {
  strlcpy(g_bad_url, url, sizeof(g_bad_url));
  g_bad_at = millis();
}

// Pick the rendition by WIDTH: the smallest image at least ART_SIZE wide, else
// the largest available. Tracks carry album.images, episodes carry images
// (and show.images).
static void pick_cover(JsonObjectConst item, char *out, size_t n) {
  out[0] = 0;
  JsonArrayConst imgs = item["album"]["images"].as<JsonArrayConst>();
  if (imgs.isNull() || imgs.size() == 0) imgs = item["images"].as<JsonArrayConst>();
  if (imgs.isNull() || imgs.size() == 0) imgs = item["show"]["images"].as<JsonArrayConst>();
  const char *fit = nullptr, *largest = nullptr;
  int fit_w = INT_MAX, largest_w = -1;
  for (JsonObjectConst img : imgs) {
    const char *u = img["url"] | "";
    if (!*u) continue;
    const int w = img["width"] | 0;  // width is nullable in the schema
    if (w >= ART_SIZE && w < fit_w) { fit = u; fit_w = w; }
    if (w > largest_w) { largest = u; largest_w = w; }
  }
  const char *pick = fit ? fit : largest;
  if (pick) strlcpy(out, pick, n);
  LOGD("art", "%u rendition(s), picked %d px wide", (unsigned)imgs.size(), fit ? fit_w : largest_w);
}

// --- JPEG decode -----------------------------------------------------------
struct CoverDecodeCtx {
  uint16_t *dst;
  int src_w, src_h;   // decoded (scaled) image size
  int off_x, off_y;   // centre-crop offsets when larger than ART_SIZE
  uint32_t blocks;
};

// Source pixel -> destination span on one axis: 1:1 with a centre crop when
// the decoded image is at least ART_SIZE, nearest-neighbour upscale otherwise.
static inline void map_axis(int s, int src, int off, int &d0, int &d1) {
  if (src >= ART_SIZE) { d0 = s - off; d1 = d0 + 1; }
  else { d0 = s * ART_SIZE / src; d1 = (s + 1) * ART_SIZE / src; }
}

static int jpeg_draw_cb(JPEGDRAW *d) {
  CoverDecodeCtx *c = (CoverDecodeCtx *)d->pUser;
  const int used_w = d->iWidthUsed > 0 ? d->iWidthUsed : d->iWidth;
  for (int r = 0; r < d->iHeight; ++r) {
    const int sy = d->y + r;
    if (sy >= c->src_h) break;
    int y0, y1;
    map_axis(sy, c->src_h, c->off_y, y0, y1);
    if (y1 <= 0 || y0 >= ART_SIZE) continue;
    if (y0 < 0) y0 = 0;
    if (y1 > ART_SIZE) y1 = ART_SIZE;
    const uint16_t *line = d->pPixels + r * d->iWidth;  // iWidth is the row pitch
    if (c->src_w >= ART_SIZE) {
      const int vx0 = max(d->x, c->off_x);
      const int vx1 = min(d->x + used_w, c->off_x + ART_SIZE);
      if (vx1 > vx0)
        for (int y = y0; y < y1; ++y)
          memcpy(c->dst + y * ART_SIZE + (vx0 - c->off_x), line + (vx0 - d->x), (vx1 - vx0) * 2);
    } else {
      for (int col = 0; col < used_w; ++col) {
        const int sx = d->x + col;
        if (sx >= c->src_w) break;
        int x0, x1;
        map_axis(sx, c->src_w, 0, x0, x1);
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1 && x < ART_SIZE; ++x) c->dst[y * ART_SIZE + x] = line[col];
      }
    }
  }
  if ((++c->blocks & 7) == 0) delay(1);  // periodic real yield: IDLE0 must run or the watchdog fires
  return 1;
}

static bool decode_cover(uint8_t *jpg, size_t len, uint16_t *dst) {
  const uint32_t t0 = millis();
  JPEGDEC &j = *g_jpeg;
  if (!j.openRAM(jpg, (int)len, jpeg_draw_cb)) {
    LOGW("jpeg", "not decodable (JPEGDEC error %d)", j.getLastError());
    return false;
  }
  const int w = j.getWidth(), h = j.getHeight();
  const bool progressive = j.getJPEGType() == JPEG_MODE_PROGRESSIVE;
  // Largest reduction that still fills ART_SIZE (1, 1/2, 1/4, 1/8).
  int shift = 0;
  if (progressive) {
    shift = 3;  // JPEGDEC decodes only the DC scan of progressive files: 1/8 size
    LOGW("jpeg", "progressive JPEG: only a 1/8 preview can be decoded, it will look soft");
  } else {
    const int m = min(w, h);
    while (shift < 3 && ((m + (2 << shift) - 1) >> (shift + 1)) >= ART_SIZE) ++shift;
  }
  static const int k_scale_opt[4] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  static CoverDecodeCtx ctx;  // net_task only
  ctx.dst = dst;
  ctx.src_w = (w + (1 << shift) - 1) >> shift;
  ctx.src_h = (h + (1 << shift) - 1) >> shift;
  ctx.off_x = ctx.src_w > ART_SIZE ? (ctx.src_w - ART_SIZE) / 2 : 0;
  ctx.off_y = ctx.src_h > ART_SIZE ? (ctx.src_h - ART_SIZE) / 2 : 0;
  ctx.blocks = 0;
  if (ctx.src_w < ART_SIZE || ctx.src_h < ART_SIZE) memset(dst, 0, ART_BYTES);

  j.setPixelType(RGB565_BIG_ENDIAN);  // matches LV_COLOR_16_SWAP 1 (set after openRAM, which resets it)
  j.setUserPointer(&ctx);
  const int ok = j.decode(0, 0, k_scale_opt[shift]);
  const int err = j.getLastError();
  j.close();
  if (!ok) {
    LOGW("jpeg", "decode failed (JPEGDEC error %d)", err);
    return false;
  }
  LOGI("jpeg", "%dx%d %s, 1/%d -> %dx%d -> %s %dx%d in %lu ms", w, h, progressive ? "progressive" : "baseline",
       1 << shift, ctx.src_w, ctx.src_h,
       (ctx.src_w == ART_SIZE && ctx.src_h == ART_SIZE) ? "exact"
       : (ctx.src_w >= ART_SIZE && ctx.src_h >= ART_SIZE) ? "crop" : "upscale", ART_SIZE, ART_SIZE,
       (unsigned long)(millis() - t0));
  (void)err;
  return true;
}

// --- download ----------------------------------------------------------------
// Sink for the rare response without Content-Length: HTTPClient::writeToStream()
// removes the chunk framing before it gets here.
class PsramSink : public Stream {
 public:
  PsramSink(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n) override {
    const size_t k = (n <= cap_ - len_) ? n : cap_ - len_;
    memcpy(buf_ + len_, p, k);
    len_ += k;
    delay(1);   // same rule as the main download loop
    return k;   // a short write makes HTTPClient report an error
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t length() const { return len_; }

 private:
  uint8_t *buf_;
  size_t cap_;
  size_t len_ = 0;
};

// Downloads a cover from i.scdn.co with its own short-lived TLS client.
// Returns a PSRAM buffer (caller frees) or nullptr. Truncated files are rejected.
static uint8_t *download_cover(const char *url, size_t *out_len) {
  *out_len = 0;
  const uint32_t t0 = millis();
  if (!tls_heap_ok("art download")) {
    LOGW("art", "closing the api.spotify.com keep-alive session to make room for this download");
    api_close();
  }
  WiFiClientSecure tls;
  tls_prepare(tls);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, url)) {
    LOGW("art", "cannot parse URL %s", url);
    return nullptr;
  }
  const char *hdrs[] = {"Content-Type"};
  http.collectHeaders(hdrs, 1);
  const int code = http.GET();
  if (code != 200) {
    char why[120] = "";
    if (code == HTTPC_ERROR_CONNECTION_REFUSED) describe_connect_error(tls, why, sizeof(why));
    LOGW("art", "GET %s -> %d %s %s", url, code, code < 0 ? HTTPClient::errorToString(code).c_str() : "", why);
    http.end();
    return nullptr;
  }
  const int len = http.getSize();  // Content-Length, -1 when absent
  const String ctype = http.header("Content-Type");
  if (ctype.length() && ctype.indexOf("jpeg") < 0 && ctype.indexOf("jpg") < 0)
    LOGW("art", "unexpected Content-Type '%s'", ctype.c_str());
  if (len == 0 || len > ART_MAX_JPEG_BYTES) {
    LOGW("art", "refusing a %d-byte image", len);
    http.end();
    return nullptr;
  }
  const size_t cap = len > 0 ? (size_t)len : ART_MAX_JPEG_BYTES;
  uint8_t *jpg = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpg) {
    LOGE("art", "no PSRAM for a %u-byte download", (unsigned)cap);
    http.end();
    return nullptr;
  }

  size_t got = 0;
  if (len > 0) {
    // Binary body of known length: read the raw socket. (JSON bodies always go
    // through getString(), which also strips chunk framing.)
    auto *s = http.getStreamPtr();
    uint32_t last_data = millis();
    while (got < (size_t)len) {
      const int avail = s->available();
      if (avail > 0) {
        const int n = s->read(jpg + got, min((size_t)avail, (size_t)len - got));
        if (n > 0) {
          got += n;
          last_data = millis();
        }
      } else if (!s->connected()) {
        break;
      } else if (millis() - last_data > HTTP_TIMEOUT_MS) {
        LOGW("art", "download stalled");
        break;
      }
      delay(1);  // unconditional: yield() would not let IDLE0 run -> task watchdog reset
    }
  } else {
    LOGI("art", "no Content-Length (chunked response), letting HTTPClient de-chunk");
    PsramSink sink(jpg, cap);
    const int r = http.writeToStream(&sink);
    got = sink.length();
    if (r < 0) {
      LOGW("art", "download failed after %u bytes: %s", (unsigned)got, HTTPClient::errorToString(r).c_str());
      got = 0;
    }
  }
  http.end();
  tls.stop();

  LOGI("art", "downloaded %u of %d bytes (Content-Length) in %lu ms", (unsigned)got, len, (unsigned long)(millis() - t0));
  if ((len > 0 && got != (size_t)len) || got == 0) {
    LOGW("art", "TRUNCATED download (%u of %d bytes) - not decoding it", (unsigned)got, len);
    heap_caps_free(jpg);
    return nullptr;
  }
  if (got < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
    LOGW("art", "not a JPEG (no SOI marker)");
    heap_caps_free(jpg);
    return nullptr;
  }
  if (jpg[got - 2] != 0xFF || jpg[got - 1] != 0xD9) LOGW("art", "no EOI marker at the end (trailing bytes?)");
  *out_len = got;
  return jpg;
}

static bool fetch_cover(const char *url, uint16_t *dst) {
  size_t len = 0;
  uint8_t *jpg = download_cover(url, &len);
  if (!jpg) return false;
  const bool ok = decode_cover(jpg, len, dst);
  heap_caps_free(jpg);
  return ok;
}

// --- hand-off to the UI ------------------------------------------------------
// g_stage_buf is ours unless a copy into g_art_buf is still pending.
static bool wait_stage_free(uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  for (;;) {
    ps_lock();
    const bool busy = g_ps.art_copy_req;
    ps_unlock();
    if (!busy) return true;
    if (millis() - t0 > timeout_ms) return false;
    delay(10);
  }
}

static void art_request_copy(const char *url) {
  ps_lock();
  g_ps.art_copy_req = true;  // ui_tick() memcpy's g_stage_buf -> g_art_buf, then sets art_shown
  ps_unlock();
  strlcpy(g_front_url, url, sizeof(g_front_url));
}

static void art_set_shown(bool shown) {
  ps_lock();
  g_ps.art_shown = shown;
  ps_unlock();
}

// Decode queue[0]'s cover into g_stage_buf so the next track change is instant.
// Called once per track change - the queue is never polled.
static JsonDocument *g_queue_filter = nullptr;
static void prefetch_next() {
  ApiResult r;
  if (!spotify_request("GET", "/me/player/queue", r)) {
    LOGW("art", "queue not available (%d: %s) - no prefetch this time", r.status, r.message);
    return;
  }
  if (!body_is_json(r.body)) {
    LOGW("art", "queue: HTTP %d without a JSON body", r.status);
    return;
  }
  JsonDocument doc(&g_json_alloc);
  const DeserializationError e = deserializeJson(doc, r.body, DeserializationOption::Filter(*g_queue_filter));
  const size_t body_len = r.body.length();
  r.body = String();  // free the (possibly large) body now
  if (e) {
    LOGW("art", "queue JSON error: %s (%u B)", e.c_str(), (unsigned)body_len);
    return;
  }
  JsonArrayConst q = doc["queue"].as<JsonArrayConst>();
  LOGD("art", "queue: %u items, %u B", (unsigned)q.size(), (unsigned)body_len);
  if (q.isNull() || q.size() == 0) {
    LOGI("art", "queue is empty - nothing to prefetch");
    return;
  }
  JsonObjectConst next = q[0];
  char name[64];
  fold_for_font(next["name"] | "?", name, sizeof(name));
  char url[256];
  pick_cover(next, url, sizeof(url));
  if (!url[0]) {
    LOGI("art", "next item \"%s\" has no artwork", name);
    return;
  }
  if (strcmp(url, g_front_url) == 0) {
    LOGI("art", "next item \"%s\" shares the current cover - nothing to prefetch", name);
    return;
  }
  if (strcmp(url, g_stage_url) == 0) {
    LOGI("art", "cover of \"%s\" is already prefetched", name);
    return;
  }
  if (url_is_bad(url)) {
    LOGI("art", "cover of \"%s\" failed recently - not prefetching", name);
    return;
  }
  if (!wait_stage_free(STAGE_WAIT_MS)) {
    LOGW("art", "display has not taken the previous cover yet - skipping prefetch");
    return;
  }
  g_stage_url[0] = 0;
  if (fetch_cover(url, g_stage_buf)) {
    strlcpy(g_stage_url, url, sizeof(g_stage_url));
    LOGI("art", "prefetched cover of next item \"%s\"", name);
  } else {
    mark_bad(url);
    LOGW("art", "prefetch of \"%s\" failed", name);
  }
}

static void service_commands();

// Runs after a track change (by id) or when the current item's cover URL has
// not been attempted yet.
static void art_run_pipeline(bool with_prefetch) {
  char url[sizeof(g_ps.art_url)];
  ps_lock();
  const bool has = g_ps.has_item;
  strlcpy(url, g_ps.art_url, sizeof(url));
  ps_unlock();
  if (!has) return;

  strlcpy(g_art_tried, url, sizeof(g_art_tried));  // record the attempt up front: success or failure, tried once

  if (!url[0]) {
    LOGI("art", "item has no artwork -> logo");
    art_set_shown(false);
  } else if (g_front_url[0] && strcmp(url, g_front_url) == 0) {
    LOGI("art", "same cover as before (same album) - nothing to fetch");
    art_set_shown(true);
  } else if (g_stage_url[0] && strcmp(url, g_stage_url) == 0) {
    ++g_art_hits;
    LOGI("art", "prefetch HIT  -> copying the prefetched cover (hits %lu / misses %lu)", (unsigned long)g_art_hits,
         (unsigned long)g_art_misses);
    art_request_copy(url);
  } else {
    ++g_art_misses;
    LOGI("art", "prefetch MISS (%s) -> downloading now (hits %lu / misses %lu)",
         g_stage_url[0] ? "a different cover was prefetched, normal after a manual skip" : "nothing prefetched",
         (unsigned long)g_art_hits, (unsigned long)g_art_misses);
    if (url_is_bad(url)) {
      LOGW("art", "this URL failed recently - not retrying, showing logo");
      art_set_shown(false);
    } else if (!wait_stage_free(STAGE_WAIT_MS)) {
      // Local hiccup (UI stalled), not a bad URL: show the logo and let the
      // next poll try this cover again.
      LOGW("art", "display has not taken the previous cover yet - will retry");
      art_set_shown(false);
      g_art_tried[0] = 0;
    } else {
      g_stage_url[0] = 0;
      if (fetch_cover(url, g_stage_buf)) {
        strlcpy(g_stage_url, url, sizeof(g_stage_url));
        art_request_copy(url);
      } else {
        mark_bad(url);
        art_set_shown(false);
        LOGW("art", "cover failed -> showing logo (%s)", url);
      }
    }
  }

  if (with_prefetch) {
    service_commands();  // stay responsive between the two downloads
    prefetch_next();
  }
}

// ============================================================================
// net_task: polling and commands
// ============================================================================
static JsonDocument *g_player_filter = nullptr;
static uint32_t g_next_poll = 0;
static uint32_t g_poll_failures = 0;
static char     g_last_poll_error[160] = "";
static uint32_t g_stat_polls = 0, g_stat_poll_ms = 0, g_stat_poll_max = 0;

// ArduinoJson filters: in a filter, array element [0] applies to EVERY element.
// (Writing [1] would silently drop the array.)
static void build_filters() {
  g_player_filter = new JsonDocument(&g_json_alloc);
  JsonDocument &f = *g_player_filter;
  f["is_playing"] = true;
  f["progress_ms"] = true;
  f["currently_playing_type"] = true;
  f["device"]["name"] = true;
  JsonObject it = f["item"].to<JsonObject>();
  it["id"] = true;
  it["uri"] = true;
  it["name"] = true;
  it["type"] = true;
  it["duration_ms"] = true;
  it["artists"][0]["name"] = true;
  it["album"]["images"][0]["url"] = true;
  it["album"]["images"][0]["width"] = true;
  it["images"][0]["url"] = true;  // EpisodeObject
  it["images"][0]["width"] = true;
  it["show"]["name"] = true;
  it["show"]["images"][0]["url"] = true;
  it["show"]["images"][0]["width"] = true;

  g_queue_filter = new JsonDocument(&g_json_alloc);
  JsonObject q = (*g_queue_filter)["queue"][0].to<JsonObject>();
  q["name"] = true;
  q["album"]["images"][0]["url"] = true;
  q["album"]["images"][0]["width"] = true;
  q["images"][0]["url"] = true;
  q["images"][0]["width"] = true;
  q["show"]["images"][0]["url"] = true;
  q["show"]["images"][0]["width"] = true;
}

static void player_set_idle(const char *title, const char *detail) {
  char d[sizeof(g_ps.idle_detail)];
  fold_for_font(detail, d, sizeof(d));
  ps_lock();
  const bool was = g_ps.has_item;
  g_ps.has_item = false;
  g_ps.playing = false;
  g_ps.item_id[0] = 0;
  g_ps.title[0] = 0;
  g_ps.artist[0] = 0;
  g_ps.art_url[0] = 0;  // clear it, or the art fetch would keep retrying
  g_ps.art_shown = false;
  g_ps.progress_ms = 0;
  g_ps.duration_ms = 0;
  strlcpy(g_ps.idle_title, title, sizeof(g_ps.idle_title));
  strlcpy(g_ps.idle_detail, d, sizeof(g_ps.idle_detail));
  ps_unlock();
  g_last_item[0] = 0;
  g_art_pending = false;
  if (was) LOGI("poll", "playback stopped -> idle (%s)", title);
}

// --- connectivity ladder ---------------------------------------------------
// Anything that stops the device from reaching api.spotify.com while Wi-Fi is
// up: first explain it in the log, then re-do DHCP/DNS, then restart.
static uint32_t g_conn_bad_since = 0;  // 0 = healthy
static bool g_conn_diag_done = false, g_conn_wifi_kicked = false;

static void conn_mark_bad() {
  if (!g_conn_bad_since) g_conn_bad_since = millis();
}

static void conn_mark_good() {
  if (g_conn_bad_since)
    LOGI("net", "connectivity restored after %lu s", (unsigned long)((millis() - g_conn_bad_since) / 1000));
  g_conn_bad_since = 0;
  g_conn_diag_done = false;
  g_conn_wifi_kicked = false;
}

static void conn_ladder() {
  if (!g_conn_bad_since) return;
  const uint32_t bad_ms = millis() - g_conn_bad_since;

  if (!g_conn_diag_done && bad_ms >= CONN_DIAG_AFTER_MS) {
    g_conn_diag_done = true;
    net_diag_dump("api.spotify.com unreachable for 20 s - network state:");
    if (WiFi.status() == WL_CONNECTED && WiFi.dnsIP(0) == IPAddress((uint32_t)0)) {
      const IPAddress gw = WiFi.gatewayIP();
      LOGW("net", "no DNS server configured (lost on a DHCP renewal?) - falling back to the gateway %s",
           gw.toString().c_str());
      WiFi.setDNS(gw);
    }
  }

  if (!g_conn_wifi_kicked && bad_ms >= CONN_WIFI_RETRY_MS && WiFi.status() == WL_CONNECTED) {
    g_conn_wifi_kicked = true;
    LOGW("net", "still unreachable after %lu s - reconnecting Wi-Fi for a fresh DHCP lease and DNS",
         (unsigned long)(bad_ms / 1000));
    notify_user("Reconnecting Wi-Fi");
    api_close();
    WiFi.disconnect();
    delay(300);
    WiFi.begin(SSID, PASSWORD);
  }

#if CONN_REBOOT_AFTER_MS > 0
  // Only when the Wi-Fi link itself is fine: if the link is down there is
  // nothing a restart can fix, and the Wi-Fi branch keeps retrying anyway.
  if (bad_ms >= CONN_REBOOT_AFTER_MS && WiFi.status() == WL_CONNECTED) {
    LOGE("net", "no connection to Spotify for %lu s - restarting the board", (unsigned long)(bad_ms / 1000));
    player_set_idle("Restarting", "No connection for 10 minutes");
    delay(1500);  // let the screen redraw and the serial buffer drain
    ESP.restart();
  }
#endif
}

static void on_poll_error(const ApiResult &r) {
  if (r.status == API_ERR_BACKOFF || r.status == API_ERR_NO_WIFI) return;  // already reported
  ++g_poll_failures;
  const char *title;
  char detail[160];
  strlcpy(detail, r.message, sizeof(detail));
  switch (r.status) {
    case API_ERR_AUTH:
      title = "Spotify sign-in failed";
      if (g_token_rejected) strlcat(detail, " - run get_refresh_token.py again", sizeof(detail));
      break;
    case 401: title = "Spotify rejected the token"; break;
    case 403: title = "Spotify refused access"; break;
    case 429: {
      uint32_t w;
      api_in_backoff(&w);
      title = "Spotify rate limit";
      snprintf(detail, sizeof(detail), "%.110s - waiting %lu s", r.message, (unsigned long)((w + 999) / 1000));
      break;
    }
    default: title = r.status < 0 ? "Network problem" : "Spotify error"; break;
  }
  LOGW("poll", "%s: %s (failure #%lu)", title, detail, (unsigned long)g_poll_failures);
  set_idle_text(title, detail);  // persistent on the idle screen
  if (strcmp(g_last_poll_error, r.message) != 0) {  // toast once per distinct error
    strlcpy(g_last_poll_error, r.message, sizeof(g_last_poll_error));
    notify_user("%s\n%s", title, detail);
  }
}

static void poll_player() {
  static char path[96] = "";
  if (!path[0])
    snprintf(path, sizeof(path), "/me/player?additional_types=episode%s%s", SPOTIFY_MARKET[0] ? "&market=" : "",
             SPOTIFY_MARKET);

  uint32_t seq_at_start;
  ps_lock();
  seq_at_start = g_ps.ui_seq;
  ps_unlock();

  const uint32_t t_start = millis();
  ApiResult r;
  const bool ok = spotify_request("GET", path, r);

  if (!ok) {
    if (is_connectivity_error(r.status)) conn_mark_bad();
    on_poll_error(r);
    uint32_t wait = 0;
    if (api_in_backoff(&wait)) g_next_poll = millis() + max<uint32_t>(wait, POLL_INTERVAL_MS);
    else g_next_poll = millis() + min<uint32_t>(POLL_INTERVAL_MS << min<uint32_t>(g_poll_failures, 4), POLL_MAX_INTERVAL_MS);
    return;
  }
  g_next_poll = t_start + POLL_INTERVAL_MS;
  conn_mark_good();
  if (g_poll_failures || g_last_poll_error[0]) LOGI("poll", "recovered after %lu failure(s)", (unsigned long)g_poll_failures);
  g_poll_failures = 0;
  g_last_poll_error[0] = 0;
  ++g_stat_polls;
  g_stat_poll_ms += r.elapsed_ms;
  if (r.elapsed_ms > g_stat_poll_max) g_stat_poll_max = r.elapsed_ms;

  // 204 = "Playback not available or active". Non-JSON bodies are ignored.
  if (r.status == 204 || !body_is_json(r.body)) {
    if (r.status != 204) LOGW("poll", "HTTP %d with a non-JSON body (%u B) - treating as idle", r.status, (unsigned)r.body.length());
    player_set_idle("Nothing playing", "Start Spotify on any device");
    return;
  }

  JsonDocument doc(&g_json_alloc);
  const DeserializationError e = deserializeJson(doc, r.body, DeserializationOption::Filter(*g_player_filter));
  if (e) {
    LOGW("poll", "JSON error: %s (%u B) - keeping previous state", e.c_str(), (unsigned)r.body.length());
    return;
  }
  const uint32_t sampled_at = millis() - r.elapsed_ms / 2;  // Spotify sampled progress mid-request

  JsonObjectConst item = doc["item"].as<JsonObjectConst>();
  const char *cpt = doc["currently_playing_type"] | "";
  if (item.isNull()) {  // item "Can be null" (ads, unknown types, private session)
    if (strcmp(cpt, "ad") == 0) player_set_idle("Advertisement", "Your music continues shortly");
    else player_set_idle("Nothing playing", "Start Spotify on any device");
    return;
  }

  const bool is_playing = doc["is_playing"] | false;
  const uint32_t progress = doc["progress_ms"] | 0;  // nullable
  const char *type = item["type"] | "track";
  const bool episode = strcmp(type, "episode") == 0;
  const char *id = item["id"] | "";
  if (!*id) id = item["uri"] | "";  // local files have a null id
  if (!*id) id = item["name"] | "";

  char title[128], artist[128], raw[256], cover[256];
  fold_for_font(item["name"] | "", title, sizeof(title));
  raw[0] = 0;
  if (episode) {
    strlcpy(raw, item["show"]["name"] | "", sizeof(raw));
  } else {
    for (JsonObjectConst a : item["artists"].as<JsonArrayConst>()) {
      const char *nm = a["name"] | "";
      if (!*nm) continue;
      if (raw[0]) strlcat(raw, ", ", sizeof(raw));
      strlcat(raw, nm, sizeof(raw));
    }
  }
  fold_for_font(raw, artist, sizeof(artist));
  pick_cover(item, cover, sizeof(cover));
  const uint32_t duration = item["duration_ms"] | 0;

  bool was_playing, kept_optimistic = false;
  ps_lock();
  was_playing = g_ps.playing;
  g_ps.has_item = true;
  strlcpy(g_ps.item_id, id, sizeof(g_ps.item_id));
  strlcpy(g_ps.title, title, sizeof(g_ps.title));
  strlcpy(g_ps.artist, artist, sizeof(g_ps.artist));
  strlcpy(g_ps.art_url, cover, sizeof(g_ps.art_url));
  g_ps.duration_ms = duration;
  if (g_ps.ui_seq == seq_at_start) {
    g_ps.playing = is_playing;
    g_ps.progress_ms = progress;
    g_ps.progress_at = sampled_at;
  } else {
    kept_optimistic = true;  // the user tapped while this request was in flight
  }
  ps_unlock();

  if (kept_optimistic) LOGD("poll", "result predates a tap - keeping the optimistic play/pause state");

  const bool track_changed = strcmp(id, g_last_item) != 0;
  if (track_changed) {
    strlcpy(g_last_item, id, sizeof(g_last_item));
    LOGI("poll", "now %s: \"%s\" - %s (%lu s, %s) on \"%s\"", is_playing ? "playing" : "paused", title, artist,
         (unsigned long)(duration / 1000), type, doc["device"]["name"] | "?");
    g_art_pending = true;
    g_prefetch_pending = true;
  } else if (!kept_optimistic && was_playing != is_playing) {
    LOGI("poll", "%s", is_playing ? "resumed" : "paused");
  }
  if (cover[0] && strcmp(cover, g_art_tried) != 0) g_art_pending = true;  // cover not attempted yet
}

static void revert_optimistic(CmdType t) {
  if (t != CMD_PLAY && t != CMD_PAUSE) return;
  const uint32_t now = millis();
  ps_lock();
  if (g_ps.playing) {  // undo an optimistic "play": freeze the position again
    uint32_t pos = g_ps.progress_ms + (now - g_ps.progress_at);
    if (g_ps.duration_ms && pos > g_ps.duration_ms) pos = g_ps.duration_ms;
    g_ps.progress_ms = pos;
  }
  g_ps.progress_at = now;
  g_ps.playing = (t == CMD_PAUSE);  // pause failed -> still playing, play failed -> still paused
  g_ps.ui_seq++;
  ps_unlock();
}

static void service_commands() {
  static const char *const k_name[] = {"Play", "Pause", "Next", "Previous"};
  static const char *const k_method[] = {"PUT", "PUT", "POST", "POST"};
  static const char *const k_path[] = {"/me/player/play", "/me/player/pause", "/me/player/next", "/me/player/previous"};
  Cmd c;
  while (xQueueReceive(g_cmd_q, &c, 0) == pdTRUE) {
    if (c.type > CMD_PREV) continue;
    ApiResult r;
    const bool ok = spotify_request(k_method[c.type], k_path[c.type], r);
    if (ok) {
      LOGI("cmd", "%s -> HTTP %d in %lu ms (%lu ms after the tap)", k_name[c.type], r.status,
           (unsigned long)r.elapsed_ms, (unsigned long)(millis() - c.issued_at));
      g_next_poll = millis() + CMD_SETTLE_MS;  // let Spotify apply it, then re-read
    } else {
      LOGW("cmd", "%s failed: %d %s", k_name[c.type], r.status, r.message);
      revert_optimistic(c.type);
      notify_user("%s failed\n%s", k_name[c.type], r.message);
      g_next_poll = millis();  // re-read the real state now (if not backing off)
    }
  }
}

static void log_stats() {
  TaskHandle_t loop_task = xTaskGetHandle("loopTask");
  LOGI("stat", "polls %lu in %lus, avg %lu ms, max %lu ms | art hits %lu misses %lu | stack free: net %u B, loop %u B",
       (unsigned long)g_stat_polls, (unsigned long)(STATS_EVERY_MS / 1000),
       (unsigned long)(g_stat_polls ? g_stat_poll_ms / g_stat_polls : 0), (unsigned long)g_stat_poll_max,
       (unsigned long)g_art_hits, (unsigned long)g_art_misses, (unsigned)uxTaskGetStackHighWaterMark(NULL),
       loop_task ? (unsigned)uxTaskGetStackHighWaterMark(loop_task) : 0U);
  (void)loop_task;
  log_heap("periodic");
  g_stat_polls = g_stat_poll_ms = g_stat_poll_max = 0;
}

static void net_task(void *) {
  LOGI("net", "net_task started on core %d", (int)xPortGetCoreID());
  tls_prepare(g_api_tls);
  g_api_http.setReuse(true);
  build_filters();
  wifi_begin();

  bool online = false;
  uint32_t offline_since = millis(), next_wifi_kick = millis() + 20000, next_stats = millis() + STATS_EVERY_MS;

  for (;;) {
    const uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      conn_mark_bad();
      if (online) {
        online = false;
        offline_since = now;
        next_wifi_kick = now + 20000;
        api_close();
        LOGW("wifi", "connection lost");
        player_set_idle("Wi-Fi lost", "Reconnecting...");
      }
      if ((int32_t)(now - next_wifi_kick) >= 0) {
        LOGW("wifi", "offline for %lu s, restarting the connection", (unsigned long)((now - offline_since) / 1000));
        WiFi.disconnect();
        WiFi.begin(SSID, PASSWORD);
        next_wifi_kick = now + 20000;
      }
      Cmd c;
      while (xQueueReceive(g_cmd_q, &c, 0) == pdTRUE) {
        revert_optimistic(c.type);
        notify_user("Offline\nWaiting for Wi-Fi");
      }
      conn_ladder();
      delay(100);
      continue;
    }

    if (!online) {
      online = true;
      LOGI("wifi", "connected in %lu ms: IP %s, RSSI %d dBm, channel %d", (unsigned long)(now - offline_since),
           WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
      log_heap("wifi up");
      set_idle_text("Signing in", "Contacting Spotify");
      g_next_poll = now;
    }

    service_commands();
    if ((int32_t)(millis() - g_next_poll) >= 0) poll_player();
    if (g_art_pending) {
      g_art_pending = false;
      const bool with_prefetch = g_prefetch_pending;
      g_prefetch_pending = false;
      art_run_pipeline(with_prefetch);
    }
    conn_ladder();
    if ((int32_t)(now - next_stats) >= 0) {
      next_stats = now + STATS_EVERY_MS;
      log_stats();
    }
    delay(20);  // real block (vTaskDelay): IDLE0 runs, the task watchdog stays fed
  }
}

// ============================================================================
// Display bring-up (core 1)
// ============================================================================
// Same sequence as scr_lvgl_init() in scr_st77916.h (kept unmodified), reusing
// its objects and callbacks. Differences: the LVGL draw buffer is 40 rows
// instead of 72 (28.8 KB instead of 51.8 KB of internal RAM, so TLS keeps a
// large contiguous block), and the backlight stays off until the first frame.
static void display_init() {
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_13_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
  ledc_channel_config_t ledc_channel = {
      .gpio_num = (TFT_BLK),
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
  backlight = new ESP_PanelBacklight(ledc_timer, ledc_channel);
  backlight->begin();
  backlight->off();

  ESP_PanelBus_I2C *touch_bus =
      new ESP_PanelBus_I2C(TOUCH_PIN_NUM_I2C_SCL, TOUCH_PIN_NUM_I2C_SDA, ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG());
  touch_bus->configI2cFreqHz(400000);
  touch_bus->begin();
  touch = new ESP_PanelTouch_CST816S(touch_bus, SCREEN_RES_HOR, SCREEN_RES_VER, TOUCH_PIN_NUM_RST, TOUCH_PIN_NUM_INT);
  touch->init();
  touch->begin();
#if TOUCH_PIN_NUM_INT >= 0
  touch->attachInterruptCallback(onTouchInterruptCallback, NULL);
#endif

  ESP_PanelBus_QSPI *panel_bus = new ESP_PanelBus_QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
  panel_bus->configQspiFreqHz(TFT_SPI_FREQ_HZ);
  panel_bus->begin();
  lcd = new ESP_PanelLcd_ST77916(panel_bus, 16, TFT_RST);
  lcd->configVendorCommands(lcd_init_cmd, sizeof(lcd_init_cmd) / sizeof(lcd_init_cmd[0]));  // before init()
  lcd->init();
  lcd->reset();
  lcd->begin();
  lcd->invertColor(true);
  lcd->displayOn();

  const size_t buf_px = (size_t)SCREEN_RES_HOR * LVGL_DRAW_ROWS;
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!disp_draw_buf) {
    LOGE("ui", "cannot allocate the %u-byte LVGL draw buffer", (unsigned)(buf_px * sizeof(lv_color_t)));
    for (;;) delay(1000);
  }
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, buf_px);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_RES_HOR;
  disp_drv.ver_res = SCREEN_RES_VER;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.user_data = (void *)lcd;
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
  if (lcd->getBus()->getType() != ESP_PANEL_BUS_TYPE_RGB) {
    lcd->attachRefreshFinishCallback(onRefreshFinishCallback, (void *)disp->driver);
  }
  indev_touchpad = indev_init(touch);
  LOGI("ui", "panel ready, LVGL draw buffer %u rows = %u B internal", LVGL_DRAW_ROWS,
       (unsigned)(buf_px * sizeof(lv_color_t)));
}

// ============================================================================
// Widgets and UI logic (core 1 only)
// ============================================================================
static lv_obj_t *ui_arc, *ui_art, *ui_title, *ui_artist, *ui_time, *ui_prev, *ui_next, *ui_tap, *ui_tap_icon, *ui_toast;

static uint32_t ui_tap_until = 0;     // tap feedback expiry, handled by ui_tick()
static uint32_t ui_toast_until = 0;
static uint32_t ui_toast_seen = 0;
static uint32_t ui_idle_since = 0;
static int8_t   ui_idle_state = -1;   // -1 unknown, 0 track view, 1 idle view
static int8_t   ui_art_state = -1;    // -1 unknown, 0 logo, 1 cover
static uint8_t  ui_backlight = 0;
static char     ui_title_txt[128], ui_artist_txt[160], ui_time_txt[32];

static void set_text_if_changed(lv_obj_t *label, char *cache, size_t n, const char *text) {
  if (strcmp(cache, text) == 0) return;  // re-setting would restart the scroll animation
  strlcpy(cache, text, n);
  lv_label_set_text(label, text);
}

static void send_cmd(CmdType t) {
  const Cmd c = {t, millis()};
  if (xQueueSend(g_cmd_q, &c, 0) != pdTRUE) LOGW("ui", "command queue full - tap dropped");
}

static void show_tap_feedback(bool to_play) {
  // Shows the state we are moving TO. Plain show/hide: no fade on an object
  // with children (the inherited opa renders as a white block on this panel).
  lv_label_set_text(ui_tap_icon, to_play ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  lv_obj_align(ui_tap_icon, LV_ALIGN_CENTER, to_play ? 3 : 0, 0);  // optical centring of the triangle
  lv_obj_clear_flag(ui_tap, LV_OBJ_FLAG_HIDDEN);
  ui_tap_until = millis() + TAP_FEEDBACK_MS;
}

static void on_art_clicked(lv_event_t *) {
  const uint32_t now = millis();
  bool to_play;
  ps_lock();
  if (!g_ps.has_item) {
    ps_unlock();
    return;
  }
  to_play = !g_ps.playing;
  if (g_ps.playing) {  // freeze the arc exactly where it is
    uint32_t pos = g_ps.progress_ms + (now - g_ps.progress_at);
    if (g_ps.duration_ms && pos > g_ps.duration_ms) pos = g_ps.duration_ms;
    g_ps.progress_ms = pos;
  }
  g_ps.progress_at = now;
  g_ps.playing = to_play;  // optimistic: the next poll corrects it if needed
  g_ps.ui_seq++;
  ps_unlock();
  LOGI("ui", "tap artwork -> %s", to_play ? "play" : "pause");
  send_cmd(to_play ? CMD_PLAY : CMD_PAUSE);
  show_tap_feedback(to_play);
}

static void on_prev_clicked(lv_event_t *) {
  LOGI("ui", "tap previous");
  send_cmd(CMD_PREV);
}

static void on_next_clicked(lv_event_t *) {
  LOGI("ui", "tap next");
  send_cmd(CMD_NEXT);
}

static lv_obj_t *make_nav_button(lv_obj_t *parent, const char *symbol, int x, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, NAV_BTN_SIZE, NAV_BTN_SIZE);  // 64 px circular hit area
  lv_obj_align(b, LV_ALIGN_CENTER, x, 0);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);  // bg_opa only - no opa/fade on a parent
  lv_obj_set_style_text_color(b, lv_color_white(), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(COLOR_SPOTIFY_GREEN), LV_STATE_PRESSED);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, symbol);  // built-in symbol: scales cleanly, no bitmap in flash
  lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static void ui_build() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Progress arc on the rim: starts at 12 o'clock, 0..1000
  ui_arc = lv_arc_create(scr);
  lv_obj_remove_style_all(ui_arc);
  lv_obj_set_size(ui_arc, ARC_DIAMETER, ARC_DIAMETER);
  lv_obj_center(ui_arc);
  lv_arc_set_rotation(ui_arc, 270);
  lv_arc_set_bg_angles(ui_arc, 0, 360);
  lv_arc_set_range(ui_arc, 0, 1000);
  lv_arc_set_value(ui_arc, 0);
  lv_obj_set_style_arc_width(ui_arc, ARC_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ui_arc, lv_color_hex(COLOR_ARC_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ui_arc, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ui_arc, false, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ui_arc, ARC_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ui_arc, lv_color_hex(COLOR_SPOTIFY_GREEN), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ui_arc, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ui_arc, true, LV_PART_INDICATOR);
  lv_obj_clear_flag(ui_arc, LV_OBJ_FLAG_CLICKABLE);  // display only, no knob

  // Album art (lv_img over a PSRAM RGB565 buffer), centred on y = 58 + 75
  ui_art = lv_img_create(scr);
  lv_img_set_src(ui_art, &logo_dsc);
  lv_obj_align(ui_art, LV_ALIGN_CENTER, 0, ART_CENTER_OFS_Y);
  lv_obj_add_event_cb(ui_art, on_art_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_clear_flag(ui_art, LV_OBJ_FLAG_CLICKABLE);  // enabled in the track view

  ui_title = lv_label_create(scr);
  lv_obj_set_width(ui_title, TEXT_WIDTH);
  lv_label_set_long_mode(ui_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(ui_title, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(ui_title, lv_color_white(), 0);
  lv_obj_set_style_text_align(ui_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_anim_speed(ui_title, 30, 0);
  lv_obj_align(ui_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
  lv_label_set_text(ui_title, "");

  ui_artist = lv_label_create(scr);
  lv_obj_set_width(ui_artist, TEXT_WIDTH);
  lv_label_set_long_mode(ui_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(ui_artist, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(ui_artist, lv_color_hex(COLOR_TEXT_GREY), 0);
  lv_obj_set_style_text_align(ui_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_anim_speed(ui_artist, 30, 0);
  lv_obj_align(ui_artist, LV_ALIGN_TOP_MID, 0, ARTIST_Y);
  lv_label_set_text(ui_artist, "");

  ui_time = lv_label_create(scr);
  lv_obj_set_width(ui_time, TEXT_WIDTH);
  lv_obj_set_style_text_font(ui_time, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_time, lv_color_hex(COLOR_TEXT_GREY), 0);
  lv_obj_set_style_text_align(ui_time, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui_time, LV_ALIGN_TOP_MID, 0, TIME_Y);
  lv_label_set_text(ui_time, "");

  ui_prev = make_nav_button(scr, LV_SYMBOL_PREV, -NAV_BTN_X, on_prev_clicked);
  ui_next = make_nav_button(scr, LV_SYMBOL_NEXT, NAV_BTN_X, on_next_clicked);

  // Tap feedback: translucent black disc over the artwork. Not clickable, so a
  // second tap still reaches the artwork underneath.
  ui_tap = lv_obj_create(scr);
  lv_obj_remove_style_all(ui_tap);
  lv_obj_set_size(ui_tap, TAP_DISC_SIZE, TAP_DISC_SIZE);
  lv_obj_align(ui_tap, LV_ALIGN_CENTER, 0, ART_CENTER_OFS_Y);
  lv_obj_set_style_radius(ui_tap, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_tap, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui_tap, LV_OPA_60, 0);
  lv_obj_clear_flag(ui_tap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  ui_tap_icon = lv_label_create(ui_tap);
  lv_obj_set_style_text_font(ui_tap_icon, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(ui_tap_icon, lv_color_white(), 0);
  lv_label_set_text(ui_tap_icon, LV_SYMBOL_PAUSE);
  lv_obj_center(ui_tap_icon);
  lv_obj_add_flag(ui_tap, LV_OBJ_FLAG_HIDDEN);

  // Toast for errors and notices (a single label, no children)
  ui_toast = lv_label_create(scr);
  lv_obj_set_width(ui_toast, 220);
  lv_label_set_long_mode(ui_toast, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ui_toast, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_toast, lv_color_white(), 0);
  lv_obj_set_style_text_align(ui_toast, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(ui_toast, lv_color_hex(0x181818), 0);
  lv_obj_set_style_bg_opa(ui_toast, LV_OPA_90, 0);
  lv_obj_set_style_border_color(ui_toast, lv_color_hex(0x3A3A3A), 0);
  lv_obj_set_style_border_width(ui_toast, 1, 0);
  lv_obj_set_style_radius(ui_toast, 12, 0);
  lv_obj_set_style_pad_all(ui_toast, 10, 0);
  lv_obj_align(ui_toast, LV_ALIGN_CENTER, 0, ART_CENTER_OFS_Y);
  lv_label_set_text(ui_toast, "");
  lv_obj_add_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);
}

// Every 100 ms on core 1: shared state -> widgets.
static void ui_tick(lv_timer_t *) {
  static PlayerState s;  // snapshot (core 1 only)
  const uint32_t now = millis();
  ps_lock();
  s = g_ps;
  ps_unlock();

  // 1. Artwork hand-off: the only place g_art_buf is written.
  bool art_changed = false;
  if (s.art_copy_req) {
    memcpy(g_art_buf, g_stage_buf, ART_BYTES);
    lv_img_cache_invalidate_src(&g_art_dsc);
    ps_lock();
    g_ps.art_copy_req = false;
    g_ps.art_shown = true;
    ps_unlock();
    s.art_shown = true;
    art_changed = true;
  }

  // 2. Idle view vs track view
  const bool idle = !s.has_item;
  if ((int8_t)idle != ui_idle_state) {
    ui_idle_state = idle;
    if (idle) {
      lv_obj_add_flag(ui_prev, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_next, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_time, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_tap, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_art, LV_OBJ_FLAG_CLICKABLE);
      lv_arc_set_value(ui_arc, 0);
      ui_tap_until = 0;
      ui_idle_since = now;
    } else {
      lv_obj_clear_flag(ui_prev, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_next, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_time, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui_art, LV_OBJ_FLAG_CLICKABLE);
    }
    // idle: long status messages scroll; track view: artist ends with "..."
    lv_label_set_long_mode(ui_artist, idle ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
    LOGI("ui", "%s view", idle ? "idle" : "track");
  }

  // 3. Cover or logo
  const int8_t want_art = (!idle && s.art_shown) ? 1 : 0;
  if (want_art != ui_art_state) {
    ui_art_state = want_art;
    lv_img_set_src(ui_art, want_art ? (const void *)&g_art_dsc : (const void *)&logo_dsc);
  } else if (art_changed && want_art) {
    lv_obj_invalidate(ui_art);
  }

  // 4. Text and progress (interpolated locally between 3 s polls)
  if (idle) {
    set_text_if_changed(ui_title, ui_title_txt, sizeof(ui_title_txt), s.idle_title);
    set_text_if_changed(ui_artist, ui_artist_txt, sizeof(ui_artist_txt), s.idle_detail);
  } else {
    set_text_if_changed(ui_title, ui_title_txt, sizeof(ui_title_txt), s.title);
    set_text_if_changed(ui_artist, ui_artist_txt, sizeof(ui_artist_txt), s.artist);
    uint32_t pos = s.progress_ms;
    if (s.playing) pos += now - s.progress_at;
    if (s.duration_ms && pos > s.duration_ms) pos = s.duration_ms;
    lv_arc_set_value(ui_arc, s.duration_ms ? (int16_t)((uint64_t)pos * 1000 / s.duration_ms) : 0);
    char a[16], b[16], t[36];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), s.duration_ms);
    snprintf(t, sizeof(t), "%s / %s", a, b);
    set_text_if_changed(ui_time, ui_time_txt, sizeof(ui_time_txt), t);
  }

  // 5. Tap feedback expiry (driven from here, not from a one-shot lv_timer)
  if (ui_tap_until && (int32_t)(now - ui_tap_until) >= 0) {
    lv_obj_add_flag(ui_tap, LV_OBJ_FLAG_HIDDEN);
    ui_tap_until = 0;
  }

  // 6. Toast
  if (s.toast_seq != ui_toast_seen) {
    ui_toast_seen = s.toast_seq;
    lv_label_set_text(ui_toast, s.toast);
    lv_obj_clear_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_toast);
    ui_toast_until = now + TOAST_MS;
  }
  if (ui_toast_until && (int32_t)(now - ui_toast_until) >= 0) {
    lv_obj_add_flag(ui_toast, LV_OBJ_FLAG_HIDDEN);
    ui_toast_until = 0;
  }

  // 7. Backlight: 10 % after 10 s idle without touch; full on a track or a touch
  uint8_t want_bl = BACKLIGHT_FULL;
  if (idle && (now - ui_idle_since) >= IDLE_DIM_AFTER_MS && lv_disp_get_inactive_time(NULL) >= IDLE_DIM_AFTER_MS)
    want_bl = BACKLIGHT_DIM;
  if (want_bl != ui_backlight) {
    ui_backlight = want_bl;
    set_brightness(want_bl);
    LOGI("ui", "backlight %u%%", (unsigned)want_bl);
  }
}

// ============================================================================
// Entry points used by setup()
// ============================================================================
static void fatal_halt(const char *msg) {
  LOGE("boot", "%s - halted", msg);
  for (;;) delay(1000);
}

// Shared state, PSRAM buffers, panel, LVGL, widgets and the 100 ms timer.
// Runs on core 1 (the Arduino loop task), which owns LVGL from here on.
static void ui_init() {
  if (xPortGetCoreID() != 1) LOGW("ui", "setup() is on core %d - set Tools > Arduino Runs On: Core 1", (int)xPortGetCoreID());
  if (!psramFound()) fatal_halt("PSRAM not found (Tools > PSRAM)");

  g_ps_mtx = xSemaphoreCreateMutex();
  g_cmd_q = xQueueCreate(8, sizeof(Cmd));
  memset(&g_ps, 0, sizeof(g_ps));
  strlcpy(g_ps.idle_title, "Connecting", sizeof(g_ps.idle_title));
  snprintf(g_ps.idle_detail, sizeof(g_ps.idle_detail), "Wi-Fi: %s", SSID);

  g_art_buf = (uint16_t *)heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_stage_buf = (uint16_t *)heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void *jpeg_mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_ps_mtx || !g_cmd_q || !g_art_buf || !g_stage_buf || !jpeg_mem) fatal_halt("out of memory at startup");
  g_jpeg = new (jpeg_mem) JPEGDEC();
  memset(&g_art_dsc, 0, sizeof(g_art_dsc));
  g_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  g_art_dsc.header.w = ART_SIZE;
  g_art_dsc.header.h = ART_SIZE;
  g_art_dsc.data_size = ART_BYTES;
  g_art_dsc.data = (const uint8_t *)g_art_buf;
  LOGI("mem", "PSRAM: 2 cover buffers x %u B + JPEG decoder %u B", (unsigned)ART_BYTES, (unsigned)sizeof(JPEGDEC));

  display_init();
  ui_build();
  lv_timer_create(ui_tick, UI_TICK_MS, NULL);
  ui_backlight = BACKLIGHT_FULL;  // what ui_tick() will want; the panel is still dark
  ui_tick(nullptr);               // sync the widgets with the initial state

  lv_refr_now(NULL);              // draw the first frame, then light the panel (no garbage flash)
  set_brightness(BACKLIGHT_FULL);
  log_heap("ui ready");
}

// Starts net_task on core 0.
static void net_start() {
  const BaseType_t ok = xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK, nullptr, 1, &g_net_task, NET_TASK_CORE);
  if (ok != pdPASS) fatal_halt("cannot create net_task");
  LOGI("net", "net_task created (stack %u B, core %d)", (unsigned)NET_TASK_STACK, NET_TASK_CORE);
}

#endif  // _SPOTIFY_UI_H_
