#ifndef _SPOTIFY_API_H_
#define _SPOTIFY_API_H_

/*
 * spotify_api.h - logging, boot diagnostics, Wi-Fi, OAuth tokens and
 * spotify_request(), the single entry point for Spotify Web API calls.
 *
 * Everything that touches the network runs on net_task (core 0). Nothing in
 * this file calls LVGL.
 *
 * Endpoints, parameters and response fields follow the Spotify OpenAPI schema
 * (https://developer.spotify.com/reference/web-api/open-api-schema.yaml):
 *   GET  /me/player            200 CurrentlyPlayingContextObject | 204 | 401 | 403 | 429
 *   GET  /me/player/queue      200 QueueObject                   | 401 | 403 | 429
 *   PUT  /me/player/play       204 | 401 | 403 | 429
 *   PUT  /me/player/pause      204 | 401 | 403 | 429
 *   POST /me/player/next       204 | 401 | 403 | 429
 *   POST /me/player/previous   204 | 401 | 403 | 429
 * Error bodies are { "error": ErrorObject { status, message } }.
 *
 * The token endpoint (POST https://accounts.spotify.com/api/token) is part of
 * the OAuth service, not the Web API schema. It answers
 * { access_token, token_type, scope, expires_in, refresh_token? } or an OAuth
 * error { error, error_description }.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_chip_info.h>
#include <stdarg.h>
#include "secrets.h"

// ============================================================================
// Logging
//
// One switch controls all serial output. Define APP_LOG_LEVEL before including
// this file (the .ino does) or change the default below:
//   0 = off (every LOGx() compiles to nothing)   1 = errors   2 = + warnings
//   3 = + info (default, enough to follow what happens)       4 = + debug
// ============================================================================
#ifndef APP_LOG_LEVEL
#define APP_LOG_LEVEL 3
#endif

static inline void app_log_nop(const char *, ...) __attribute__((format(printf, 1, 2)));
static inline void app_log_nop(const char *, ...) {}

#if APP_LOG_LEVEL > 0
static void app_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void app_log(char level, const char *tag, const char *fmt, ...) {
  char line[320];
  const uint32_t ms = millis();
  int n = snprintf(line, sizeof(line), "[%6lu.%03lu][%c][c%d][%-4s] ", (unsigned long)(ms / 1000),
                   (unsigned long)(ms % 1000), level, (int)xPortGetCoreID(), tag);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  va_end(ap);
  Serial.println(line);  // one write per line so the two cores do not interleave mid-line
}
#endif

#if APP_LOG_LEVEL >= 1
#define LOGE(tag, fmt, ...) app_log('E', tag, fmt, ##__VA_ARGS__)
#else
#define LOGE(tag, fmt, ...) do { if (0) app_log_nop(fmt, ##__VA_ARGS__); } while (0)
#endif
#if APP_LOG_LEVEL >= 2
#define LOGW(tag, fmt, ...) app_log('W', tag, fmt, ##__VA_ARGS__)
#else
#define LOGW(tag, fmt, ...) do { if (0) app_log_nop(fmt, ##__VA_ARGS__); } while (0)
#endif
#if APP_LOG_LEVEL >= 3
#define LOGI(tag, fmt, ...) app_log('I', tag, fmt, ##__VA_ARGS__)
#else
#define LOGI(tag, fmt, ...) do { if (0) app_log_nop(fmt, ##__VA_ARGS__); } while (0)
#endif
#if APP_LOG_LEVEL >= 4
#define LOGD(tag, fmt, ...) app_log('D', tag, fmt, ##__VA_ARGS__)
#else
#define LOGD(tag, fmt, ...) do { if (0) app_log_nop(fmt, ##__VA_ARGS__); } while (0)
#endif

// ============================================================================
// Configuration
// ============================================================================
#define SPOTIFY_API_BASE        "https://api.spotify.com/v1"
#define SPOTIFY_TOKEN_URL       "https://accounts.spotify.com/api/token"
#define SPOTIFY_REQUIRED_SCOPES "user-read-playback-state user-modify-playback-state user-read-currently-playing"

// Optional ISO 3166-1 alpha-2 code sent as `market` on GET /me/player (e.g. "FR").
// Empty = parameter omitted. The account's own country takes priority anyway.
#define SPOTIFY_MARKET          ""

#define HTTP_TIMEOUT_MS         8000     // connect + read timeout for every request
#define TLS_HANDSHAKE_TIMEOUT_S 10
#define TOKEN_EARLY_REFRESH_S   60       // renew this long before expires_in runs out

#define BACKOFF_START_MS        2000     // first 429 / 5xx back-off step
#define BACKOFF_MAX_MS          120000   // cap for the exponential back-off
#define TOKEN_RETRY_START_MS    5000     // network failure while refreshing the token
#define TOKEN_RETRY_MAX_MS      300000
#define TOKEN_REJECTED_RETRY_MS 600000   // refresh token rejected: retry slowly (10 min)

#define TLS_MIN_INTERNAL_BLOCK  (46 * 1024)  // a TLS session wants ~40-50 KB in one piece

// Self-healing ladder when the API becomes unreachable while Wi-Fi is still up
#define CONN_DIAG_AFTER_MS      20000    // dump the network state + a DNS test (once per outage)
#define CONN_WIFI_RETRY_MS      90000    // then force a full Wi-Fi reconnect (fresh DHCP lease and DNS)
#define CONN_REBOOT_AFTER_MS    600000   // last resort: restart the board. 0 disables it

// ============================================================================
// Heap / boot diagnostics
// ============================================================================
static void log_heap(const char *where) {
  LOGI("heap", "%-14s internal free %6u B, largest block %6u B, min-ever %6u B | PSRAM free %7u B",
       where, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  (void)where;
}

static const char *reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (check power supply)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "other / unknown";
  }
}

static const char *flash_mode_str(FlashMode_t m) {
  switch (m) {
    case FM_QIO:       return "QIO";
    case FM_QOUT:      return "QOUT";
    case FM_DIO:       return "DIO";
    case FM_DOUT:      return "DOUT";
    case FM_FAST_READ: return "FAST_READ";
    case FM_SLOW_READ: return "SLOW_READ";
    default:           return "unknown";
  }
}

static void print_partition_table() {
  LOGI("boot", "partition table:");
  LOGI("boot", "  %-10s %-4s %-8s %10s %10s", "label", "type", "subtype", "offset", "size");
  const esp_partition_type_t types[2] = {ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA};
  for (int t = 0; t < 2; ++t) {
    esp_partition_iterator_t it = esp_partition_find(types[t], ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
      const esp_partition_t *p = esp_partition_get(it);
      LOGI("boot", "  %-10s %-4s 0x%02x     0x%08lx %7lu KB", p->label, p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
           p->subtype, (unsigned long)p->address, (unsigned long)(p->size / 1024));
      (void)p;
      it = esp_partition_next(it);  // releases the iterator itself when it returns NULL
    }
  }
}

static void print_boot_diagnostics() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const uint32_t sketch = ESP.getSketchSize();
  const uint32_t part = running ? running->size : 0;
  LOGI("boot", "================ SpotifyESP32Gui v2 ================");
  LOGI("boot", "chip        %s rev %u, %u cores @ %lu MHz", ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
       (unsigned)ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
  LOGI("boot", "flash       %lu MB, %s @ %lu MHz", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
       flash_mode_str(ESP.getFlashChipMode()), (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  LOGI("boot", "sketch      %lu KB of %lu KB app partition '%s' (%lu%%)", (unsigned long)(sketch / 1024),
       (unsigned long)(part / 1024), running ? running->label : "?",
       part ? (unsigned long)((uint64_t)sketch * 100 / part) : 0UL);
  LOGI("boot", "reset       %s", reset_reason_str(esp_reset_reason()));
  LOGI("boot", "versions    arduino-esp32 %d.%d.%d, IDF %s, ArduinoJson %s", ESP_ARDUINO_VERSION_MAJOR,
       ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH, ESP.getSdkVersion(), ARDUINOJSON_VERSION);
  LOGI("boot", "internal    free %u B, largest free block %u B  <- TLS needs ~40-50 KB in ONE block",
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  LOGI("boot", "PSRAM       %s, %lu KB total, %lu KB free", psramFound() ? "found" : "NOT FOUND",
       (unsigned long)(ESP.getPsramSize() / 1024), (unsigned long)(ESP.getFreePsram() / 1024));
  print_partition_table();
  if (esp_reset_reason() == ESP_RST_TASK_WDT)
    LOGW("boot", "last reset was the task watchdog: a task on core 0 starved IDLE0 (missing delay())");
  (void)sketch;
  (void)part;
}

// ============================================================================
// PSRAM allocator for ArduinoJson, so parsing never fragments internal RAM
// ============================================================================
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramJsonAllocator g_json_alloc;

// Only hand a body to ArduinoJson when it actually looks like JSON. Player
// endpoints answer 204 with no body, and occasionally with non-JSON text.
static bool body_is_json(const String &b) {
  for (size_t i = 0; i < b.length(); ++i) {
    const char c = b[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    return c == '{' || c == '[';
  }
  return false;
}

// ============================================================================
// TLS: the core's bundled root store (no pinned certificate)
// ============================================================================
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// Must run before EVERY new handshake on a given WiFiClientSecure, not just
// once: in arduino-esp32 3.0.x-3.3.x, stop() ends in stop_ssl_socket(), which
// memset()s the whole ssl context and wipes the CA-bundle hook installed by
// setCACertBundle() while _use_ca_bundle stays true. The next connect() on the
// same object then handshakes with no root CAs, fails verification in ~100 ms
// and HTTPClient reports it as "connection refused" - forever. (Fixed only on
// the core's master branch.) Fresh short-lived clients are unaffected.
static void tls_prepare(WiFiClientSecure &c) {
  c.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
  c.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
}

// HTTPClient reports every failed connect (DNS, TCP or TLS) as "connection
// refused". The TLS layer keeps the real reason; lastError() survives stop().
static void describe_connect_error(WiFiClientSecure &c, char *out, size_t n) {
  char tls[100];
  const int e = c.lastError(tls, sizeof(tls));
  if (e == -1) snprintf(out, n, "TCP connect failed");
  else if (e < 0) snprintf(out, n, "TLS error -0x%04X: %.80s", -e, tls);
  else snprintf(out, n, "DNS lookup or TCP connect failed");
}

// Warn (and give the caller a chance to react) before a new TLS handshake
// when internal RAM is too fragmented to hold another session.
static bool tls_heap_ok(const char *who) {
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (largest < TLS_MIN_INTERNAL_BLOCK) {
    LOGW("heap", "%s: largest internal block only %u B (free %u B) - TLS handshake may fail", who,
         (unsigned)largest, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return false;
  }
  return true;
}

// Logs everything needed to tell a DNS problem from a TCP/TLS one.
static void net_diag_dump(const char *why) {
  LOGW("net", "%s", why);
  LOGW("net", "  wifi %s, IP %s, gateway %s, mask %s", WiFi.status() == WL_CONNECTED ? "connected" : "DOWN",
       WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.subnetMask().toString().c_str());
  LOGW("net", "  DNS %s / %s, RSSI %d dBm, channel %d", WiFi.dnsIP(0).toString().c_str(),
       WiFi.dnsIP(1).toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
  IPAddress ip;
  const uint32_t t0 = millis();
  const bool ok = WiFi.hostByName("api.spotify.com", ip) == 1;
  if (ok)
    LOGW("net", "  DNS test: api.spotify.com -> %s in %lu ms (name resolution works; the failure is TCP or TLS)",
         ip.toString().c_str(), (unsigned long)(millis() - t0));
  else
    LOGW("net", "  DNS test: api.spotify.com FAILED after %lu ms (name resolution is the problem)",
         (unsigned long)(millis() - t0));
}

// ============================================================================
// Wi-Fi
// ============================================================================
static void wifi_begin() {
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      LOGW("wifi", "disconnected, reason %u", (unsigned)info.wifi_sta_disconnected.reason);
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      LOGI("wifi", "got IP %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    (void)info;
  });
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);        // modem sleep adds 100+ ms to every request
  WiFi.setAutoReconnect(true);
  LOGI("wifi", "connecting to \"%s\"", SSID);
  WiFi.begin(SSID, PASSWORD);
}

// ============================================================================
// API state (net_task only)
// ============================================================================
static WiFiClientSecure g_api_tls;   // ONE persistent TLS session to api.spotify.com
static HTTPClient g_api_http;        // ...driven by one HTTPClient with setReuse(true)

static char     g_access_token[512];
static bool     g_token_valid = false;
static uint32_t g_token_deadline = 0;       // millis() at which we renew (expires_in - 60 s)
static uint32_t g_token_retry_at = 0;       // no refresh attempts before this millis()
static uint32_t g_token_backoff_ms = 0;
static bool     g_token_rejected = false;   // Spotify said the refresh token / client is invalid
static char     g_token_error[160] = "";    // last token failure, human readable
static String   g_refresh_token = REFRESH_TOKEN;

static uint32_t g_api_session_at = 0;       // millis() when the current keep-alive session was opened
static uint32_t g_backoff_until = 0;        // 429 / 5xx: no API traffic before this millis()
static uint32_t g_backoff_ms = 0;           // current exponential step, 0 = not backing off

// Local (negative) status codes, next to HTTPClient's own HTTPC_ERROR_* (-1..-11)
enum : int {
  API_ERR_NO_WIFI = -100,       // Wi-Fi down, request not sent
  API_ERR_AUTH = -101,          // no usable access token
  API_ERR_BACKOFF = -102,       // inside a 429/5xx back-off window, request not sent
};

struct ApiResult {
  int status = 0;               // HTTP status, or HTTPC_ERROR_* / API_ERR_* (< 0)
  String body;                  // response body ("" for 204)
  char message[160] = "";       // Spotify's error.message (or ours) when !ok()
  uint32_t elapsed_ms = 0;
  uint32_t retry_after_s = 0;   // from Retry-After on 429
  bool ok() const { return status >= 200 && status < 300; }
};

static void api_close() {
  if (g_api_tls.connected()) LOGD("api", "closing keep-alive session");
  g_api_tls.stop();
  g_api_session_at = 0;
}

static inline bool is_connectivity_error(int status) {  // DNS / TCP / TLS / no Wi-Fi, not an HTTP or auth error
  return status == API_ERR_NO_WIFI || (status < 0 && status != API_ERR_AUTH && status != API_ERR_BACKOFF);
}

static bool api_in_backoff(uint32_t *remaining_ms = nullptr) {
  const int32_t left = (int32_t)(g_backoff_until - millis());
  if (remaining_ms) *remaining_ms = left > 0 ? (uint32_t)left : 0;
  return left > 0;
}

// Exponential back-off that also honours Retry-After (seconds).
static void api_start_backoff(uint32_t retry_after_s, int status) {
  g_backoff_ms = g_backoff_ms ? min<uint32_t>(g_backoff_ms * 2, BACKOFF_MAX_MS) : BACKOFF_START_MS;
  const uint32_t wait = max<uint32_t>(retry_after_s * 1000UL, g_backoff_ms);
  g_backoff_until = millis() + wait;
  LOGW("api", "HTTP %d: backing off %lu ms (Retry-After %lu s, exponential step %lu ms)", status,
       (unsigned long)wait, (unsigned long)retry_after_s, (unsigned long)g_backoff_ms);
}

static String url_encode(const String &s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    const uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Pull a human-readable message out of an error body. Web API errors are
// { "error": { "status", "message" } }; OAuth errors are
// { "error": "<code>", "error_description": "..." }.
static void extract_error_message(const String &body, char *out, size_t n) {
  out[0] = 0;
  if (!body_is_json(body)) return;
  JsonDocument doc(&g_json_alloc);
  if (deserializeJson(doc, body)) return;
  JsonVariantConst err = doc["error"];
  if (err.is<JsonObjectConst>()) {
    strlcpy(out, err["message"] | "", n);
  } else if (err.is<const char *>()) {
    const char *code = err.as<const char *>();
    const char *desc = doc["error_description"] | "";
    snprintf(out, n, "%s%s%s", code, *desc ? ": " : "", desc);
  }
}

// ============================================================================
// Access token (refresh-token grant; the authorization-code exchange happens
// once on a PC, see extras/get_refresh_token.py)
// ============================================================================
static bool refresh_access_token() {
  const uint32_t t0 = millis();
  LOGI("auth", "refreshing access token");
  tls_heap_ok("token refresh");

  WiFiClientSecure tls;              // short-lived: runs about once an hour
  tls_prepare(tls);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, SPOTIFY_TOKEN_URL)) {
    snprintf(g_token_error, sizeof(g_token_error), "Could not start token request");
    g_token_retry_at = millis() + TOKEN_RETRY_START_MS;
    return false;
  }
  const char *hdrs[] = {"Retry-After"};
  http.collectHeaders(hdrs, 1);
  http.addHeader("Authorization", "Basic " AUTH_B64);   // precomputed base64(client_id:client_secret)
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String form = "grant_type=refresh_token&refresh_token=" + url_encode(g_refresh_token);
  const int code = http.POST(form);
  String body = (code > 0 && code != 204) ? http.getString() : String();
  const uint32_t retry_after = (uint32_t)http.header("Retry-After").toInt();
  http.end();
  tls.stop();

  if (code == 200 && body_is_json(body)) {
    JsonDocument doc(&g_json_alloc);
    DeserializationError e = deserializeJson(doc, body);
    const char *tok = doc["access_token"] | "";
    const uint32_t expires_in = doc["expires_in"] | 3600;
    if (!e && *tok && strlen(tok) < sizeof(g_access_token)) {
      strlcpy(g_access_token, tok, sizeof(g_access_token));
      const uint32_t life_s = expires_in > 2 * TOKEN_EARLY_REFRESH_S ? expires_in - TOKEN_EARLY_REFRESH_S : expires_in / 2;
      g_token_deadline = millis() + life_s * 1000UL;
      g_token_valid = true;
      g_token_rejected = false;
      g_token_backoff_ms = 0;
      g_token_error[0] = 0;
      LOGI("auth", "access token OK (%s, expires in %lu s, renewing in %lu s) [%lu ms]", doc["token_type"] | "?",
           (unsigned long)expires_in, (unsigned long)life_s, (unsigned long)(millis() - t0));

      const char *scope = doc["scope"] | "";
      const char *needed[] = {"user-read-playback-state", "user-modify-playback-state", "user-read-currently-playing"};
      for (const char *s : needed)
        if (!strstr(scope, s)) LOGW("auth", "token is missing scope '%s' - re-run the PC helper", s);

      const char *new_rt = doc["refresh_token"] | "";
      if (*new_rt && g_refresh_token != new_rt) {
        g_refresh_token = new_rt;  // Spotify rotated it: keep using the new one until reboot
        LOGW("auth", "Spotify returned a new refresh token (%.8s...). Using it until reboot; if the old one stops "
                     "working, run the PC helper again and update secrets.h.", new_rt);
      }
      return true;
    }
    snprintf(g_token_error, sizeof(g_token_error), "Unreadable token response");
  } else {
    char msg[120];
    extract_error_message(body, msg, sizeof(msg));
    if (code == 400 || code == 401) {
      // invalid_grant = refresh token expired (6-month lifetime), revoked or wrong;
      // invalid_client = AUTH_B64 is wrong. Retrying quickly will not help.
      g_token_rejected = true;
      snprintf(g_token_error, sizeof(g_token_error), "%s", *msg ? msg : "credentials rejected");
      g_token_retry_at = millis() + TOKEN_REJECTED_RETRY_MS;
      LOGE("auth", "token refresh rejected (HTTP %d): %s", code, g_token_error);
      if (strstr(msg, "invalid_grant"))
        LOGE("auth", "-> refresh token expired or revoked: run extras/get_refresh_token.py and update secrets.h");
      else if (strstr(msg, "invalid_client"))
        LOGE("auth", "-> client id/secret rejected: check AUTH_B64 in secrets.h");
      g_token_valid = false;
      return false;
    }
    if (code == 429) {
      snprintf(g_token_error, sizeof(g_token_error), "Rate limited, retrying in %lu s", (unsigned long)max<uint32_t>(retry_after, 5));
      g_token_retry_at = millis() + max<uint32_t>(retry_after, 5) * 1000UL;
      LOGW("auth", "token endpoint HTTP 429, Retry-After %lu s", (unsigned long)retry_after);
      g_token_valid = false;
      return false;
    }
    if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
      char why[120];
      describe_connect_error(tls, why, sizeof(why));
      snprintf(g_token_error, sizeof(g_token_error), "Cannot connect to accounts.spotify.com (%.100s)", why);
    } else if (code < 0)
      snprintf(g_token_error, sizeof(g_token_error), "Network error: %s", HTTPClient::errorToString(code).c_str());
    else
      snprintf(g_token_error, sizeof(g_token_error), "HTTP %d%s%s", code, *msg ? ": " : "", msg);
  }

  // Transient failure: exponential retry
  g_token_backoff_ms = g_token_backoff_ms ? min<uint32_t>(g_token_backoff_ms * 2, TOKEN_RETRY_MAX_MS) : TOKEN_RETRY_START_MS;
  g_token_retry_at = millis() + g_token_backoff_ms;
  g_token_valid = false;
  LOGW("auth", "token refresh failed (%s), next try in %lu s", g_token_error, (unsigned long)(g_token_backoff_ms / 1000));
  return false;
}

// Proactive renewal: the token is replaced before its deadline, so requests
// normally never see a 401.
static bool ensure_access_token() {
  if (g_token_valid && (int32_t)(millis() - g_token_deadline) < 0) return true;
  if (g_token_valid) LOGI("auth", "access token reached its renewal deadline");
  g_token_valid = false;
  if ((int32_t)(millis() - g_token_retry_at) < 0) return false;
  return refresh_access_token();
}

// ============================================================================
// spotify_request(): every Web API call goes through here.
//   method           "GET" / "PUT" / "POST"
//   path_and_query   e.g. "/me/player?additional_types=episode"
//   json_body        optional request body
// Returns true for any 2xx (pause/play answer 204). On failure res.status and
// res.message say what happened.
// ============================================================================
static bool spotify_request(const char *method, const char *path_and_query, ApiResult &res, const char *json_body = nullptr) {
  res = ApiResult();

  if (WiFi.status() != WL_CONNECTED) {
    res.status = API_ERR_NO_WIFI;
    strlcpy(res.message, "Wi-Fi not connected", sizeof(res.message));
    return false;
  }
  uint32_t wait_ms;
  if (api_in_backoff(&wait_ms)) {  // never retry inside the back-off window
    res.status = API_ERR_BACKOFF;
    snprintf(res.message, sizeof(res.message), "Spotify asked us to slow down, retrying in %lu s",
             (unsigned long)((wait_ms + 999) / 1000));
    return false;
  }

  const bool is_get = strcmp(method, "GET") == 0;
  const size_t body_len = json_body ? strlen(json_body) : 0;
  bool auth_retry_done = false;
  bool net_retry_done = false;

  for (;;) {
    if (!ensure_access_token()) {
      res.status = API_ERR_AUTH;
      snprintf(res.message, sizeof(res.message), "%s", *g_token_error ? g_token_error : "No access token");
      return false;
    }

    const bool reused = g_api_tls.connected();
    if (!reused) {
      if (g_api_session_at)
        LOGI("api", "previous keep-alive session lasted %lu s before it closed",
             (unsigned long)((millis() - g_api_session_at) / 1000));
      g_api_session_at = 0;
      tls_prepare(g_api_tls);  // re-arm the CA bundle: a previous stop() wiped it (see tls_prepare)
      tls_heap_ok("api handshake");
    }
    const uint32_t t0 = millis();
    res.body = String();  // a retry must not inherit the previous attempt's body
    res.retry_after_s = 0;

    String url = SPOTIFY_API_BASE;
    url += path_and_query;
    if (!g_api_http.begin(g_api_tls, url)) {
      res.status = HTTPC_ERROR_CONNECTION_REFUSED;
      strlcpy(res.message, "Could not start request", sizeof(res.message));
      return false;
    }
    g_api_http.setReuse(true);
    g_api_http.setTimeout(HTTP_TIMEOUT_MS);
    g_api_http.setConnectTimeout(HTTP_TIMEOUT_MS);
    g_api_http.setUserAgent("SpotifyESP32Gui/2");
    const char *hdrs[] = {"Retry-After"};
    g_api_http.collectHeaders(hdrs, 1);  // re-arming also clears the previous response's value
    g_api_http.addHeader("Authorization", String("Bearer ") + g_access_token);
    if (body_len) {
      g_api_http.addHeader("Content-Type", "application/json");
    } else if (!is_get) {
      g_api_http.addHeader("Content-Length", "0");  // PUT/POST without body: Spotify wants an explicit 0
    }

    const int code = g_api_http.sendRequest(method, (uint8_t *)json_body, body_len);
    if (code > 0) {
      // 204 has no body: calling getString() would wait for the socket to close.
      if (code != 204 && code != 304) res.body = g_api_http.getString();
      res.retry_after_s = (uint32_t)g_api_http.header("Retry-After").toInt();
    }
    g_api_http.end();  // with setReuse(true) the TLS session stays open
    res.elapsed_ms = millis() - t0;

    if (code < 0) {
      char why[120] = "";
      if (code == HTTPC_ERROR_CONNECTION_REFUSED) describe_connect_error(g_api_tls, why, sizeof(why));
      LOGW("api", "%s %s -> %s%s%s%s (%lu ms, %s)", method, path_and_query, HTTPClient::errorToString(code).c_str(),
           *why ? " [" : "", why, *why ? "]" : "", (unsigned long)res.elapsed_ms, reused ? "reused session" : "new session");
      api_close();
      // A kept-alive session may have been closed by the server. Retry once on a
      // fresh connection when that is safe (GET, or the request never went out).
      const bool not_sent = code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
                            code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED;
      if (reused && !net_retry_done && (is_get || not_sent)) {
        net_retry_done = true;
        LOGI("api", "retrying once on a fresh TLS session");
        continue;
      }
      res.status = code;
      if (*why) snprintf(res.message, sizeof(res.message), "Cannot connect to api.spotify.com (%.100s)", why);
      else snprintf(res.message, sizeof(res.message), "Network error: %s", HTTPClient::errorToString(code).c_str());
      return false;
    }

    res.status = code;
    LOGD("api", "%s %s -> %d, %u B, %lu ms (%s)", method, path_and_query, code, res.body.length(),
         (unsigned long)res.elapsed_ms, reused ? "keep-alive" : "new TLS session");
    if (!reused) {  // should appear rarely; in between, every request reuses the session (100-200 ms)
      g_api_session_at = millis();
      LOGI("api", "new TLS session to api.spotify.com: %s %s took %lu ms incl. handshake", method, path_and_query,
           (unsigned long)res.elapsed_ms);
    }

    // 401: token expired/revoked early. Keyed on the status code only.
    if (code == 401 && !auth_retry_done) {
      auth_retry_done = true;
      char msg[120];
      extract_error_message(res.body, msg, sizeof(msg));
      LOGW("api", "401 on %s %s (%s) -> refreshing token, retrying once", method, path_and_query, msg);
      g_token_valid = false;
      g_token_retry_at = 0;
      continue;
    }
    break;
  }

  if (res.ok()) {
    g_backoff_ms = 0;
    if (!g_api_tls.connected()) LOGD("api", "server closed the session (no keep-alive this time)");
    return true;
  }

  extract_error_message(res.body, res.message, sizeof(res.message));
  if (!res.message[0]) snprintf(res.message, sizeof(res.message), "HTTP %d", res.status);

  if (res.status == 429 || res.status >= 500) {
    api_start_backoff(res.retry_after_s, res.status);
  }
  if (res.status == 401) {
    g_token_valid = false;  // still 401 with a brand-new token: make the next call refresh again
  }
  LOGW("api", "%s %s -> HTTP %d: %s", method, path_and_query, res.status, res.message);
  return false;
}

#endif  // _SPOTIFY_API_H_
