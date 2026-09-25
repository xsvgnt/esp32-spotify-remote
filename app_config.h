#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

/*
 * app_config.h - credentials kept in NVS (flash), not compiled into the sketch.
 *
 * Stored: Wi-Fi SSID/password, Spotify client id/secret (plus the base64 of
 * "id:secret", computed once here so requests never do it), the refresh token
 * obtained by the on-device authorization, the screen brightness, and the night
 * mode window with its time zone.
 *
 * Everything lives in ONE NVS blob with a magic number, a version and a CRC.
 * NVS writes a new entry before invalidating the old one, so a single-key write
 * is atomic: a power cut leaves either the previous complete record or the new
 * one, never a mixture (which several separate keys could produce - Wi-Fi name
 * stored without its password, say). A record that fails its CRC is ignored,
 * and the board starts its setup portal.
 *
 * Nothing is compiled into the firmware: a freshly flashed board has an empty
 * record and starts its setup portal, so the same binary works for anybody.
 *
 * NVS is NOT encrypted: anyone with a USB cable and esptool can read this out
 * of flash unless you enable flash encryption (an irreversible efuse change).
 * The Wi-Fi password is in NVS anyway - the Wi-Fi stack stores its own copy.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#define CFG_NAMESPACE  "spotcfg"
#define CFG_BLOB_KEY   "cfg"
#define CFG_BLOB_MAGIC 0x53504F54UL  // "SPOT"
#define CFG_BLOB_VER   3             // 2 added the brightness, 3 the night mode and time zone

// Screen brightness: a percentage the slider moves in steps of 10.
#define CFG_BRIGHT_MIN     10
#define CFG_BRIGHT_MAX     100
#define CFG_BRIGHT_STEP    10
#define CFG_BRIGHT_DEFAULT 100

// Night mode: whole hours, local time. Start == end means "no night".
#define CFG_NIGHT_START_DEFAULT 23
#define CFG_NIGHT_END_DEFAULT   7
#define CFG_TZ_DEFAULT          "UTC0"  // a POSIX TZ rule, DST included

static uint8_t cfg_clamp_brightness(int v) {
  v = (v + CFG_BRIGHT_STEP / 2) / CFG_BRIGHT_STEP * CFG_BRIGHT_STEP;  // snap to a step
  if (v < CFG_BRIGHT_MIN) v = CFG_BRIGHT_MIN;
  if (v > CFG_BRIGHT_MAX) v = CFG_BRIGHT_MAX;
  return (uint8_t)v;
}

static uint8_t cfg_clamp_hour(int h) { return (uint8_t)(h < 0 || h > 23 ? 0 : h); }

struct AppConfig {
  char ssid[33];
  char pass[65];
  char client_id[64];
  char client_secret[96];
  char auth_b64[224];      // base64("client_id:client_secret")
  char refresh_token[320];
  uint8_t brightness;      // 10..100 %
  uint8_t night_on;        // night mode enabled
  uint8_t night_start;     // 0..23, local time
  uint8_t night_end;       // 0..23, local time (may be before the start: the window wraps)
  char tz[48];             // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"

  bool has_wifi() const { return ssid[0] != 0; }
  bool has_client() const { return client_id[0] != 0 && auth_b64[0] != 0; }
  bool has_token() const { return refresh_token[0] != 0; }
  // A window of zero length is no window at all.
  bool night_set() const { return night_on && night_start != night_end; }
};

// Is this hour inside the night window? The window may wrap past midnight
// (23 -> 7 means 23, 0, 1 ... 6), and the end hour itself is already daytime.
static bool cfg_hour_in_night(const AppConfig &c, int hour) {
  if (!c.night_set() || hour < 0 || hour > 23) return false;
  if (c.night_start < c.night_end) return hour >= c.night_start && hour < c.night_end;
  return hour >= c.night_start || hour < c.night_end;
}

// On-flash layout: packed and versioned, so it does not depend on how the
// compiler happens to lay out AppConfig. Each version has its own size, which
// is what config_read_blob() dispatches on.
struct __attribute__((packed)) ConfigBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  char ssid[33];
  char pass[65];
  char client_id[64];
  char client_secret[96];
  char auth_b64[224];
  char refresh_token[320];
  uint8_t brightness;  // added in version 2
  uint8_t pad[3];
  uint8_t night_on;    // added in version 3
  uint8_t night_start;
  uint8_t night_end;
  uint8_t pad2;
  char tz[48];
  uint32_t crc;  // over every byte before this field
};

// Older layouts, kept so a board provisioned by an earlier build keeps its
// refresh token instead of falling back to setup.
struct __attribute__((packed)) ConfigBlobV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  char ssid[33];
  char pass[65];
  char client_id[64];
  char client_secret[96];
  char auth_b64[224];
  char refresh_token[320];
  uint32_t crc;
};

struct __attribute__((packed)) ConfigBlobV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  char ssid[33];
  char pass[65];
  char client_id[64];
  char client_secret[96];
  char auth_b64[224];
  char refresh_token[320];
  uint8_t brightness;
  uint8_t pad[3];
  uint32_t crc;
};

static AppConfig g_cfg;
static Preferences g_prefs;

static uint32_t cfg_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// base64("id:secret") - done once when the credentials are saved, never per request.
static bool config_make_auth_b64(const char *id, const char *secret, char *out, size_t n) {
  char pair[sizeof(g_cfg.client_id) + sizeof(g_cfg.client_secret) + 2];
  const int len = snprintf(pair, sizeof(pair), "%s:%s", id, secret);
  size_t olen = 0;
  const int rc = mbedtls_base64_encode((unsigned char *)out, n, &olen, (const unsigned char *)pair, len);
  memset(pair, 0, sizeof(pair));  // don't leave the secret on the stack
  if (rc != 0 || olen >= n) {
    out[0] = 0;
    return false;
  }
  out[olen] = 0;
  return true;
}

// The settings a board with nothing stored starts from.
static void config_defaults() {
  g_cfg.brightness = CFG_BRIGHT_DEFAULT;
  g_cfg.night_on = 0;
  g_cfg.night_start = CFG_NIGHT_START_DEFAULT;
  g_cfg.night_end = CFG_NIGHT_END_DEFAULT;
  strlcpy(g_cfg.tz, CFG_TZ_DEFAULT, sizeof(g_cfg.tz));
}

// Single atomic write of the whole record.
static bool config_store() {
  ConfigBlob b;
  memset(&b, 0, sizeof(b));
  b.magic = CFG_BLOB_MAGIC;
  b.version = CFG_BLOB_VER;
  strlcpy(b.ssid, g_cfg.ssid, sizeof(b.ssid));
  strlcpy(b.pass, g_cfg.pass, sizeof(b.pass));
  strlcpy(b.client_id, g_cfg.client_id, sizeof(b.client_id));
  strlcpy(b.client_secret, g_cfg.client_secret, sizeof(b.client_secret));
  strlcpy(b.auth_b64, g_cfg.auth_b64, sizeof(b.auth_b64));
  strlcpy(b.refresh_token, g_cfg.refresh_token, sizeof(b.refresh_token));
  b.brightness = cfg_clamp_brightness(g_cfg.brightness);
  b.night_on = g_cfg.night_on ? 1 : 0;
  b.night_start = cfg_clamp_hour(g_cfg.night_start);
  b.night_end = cfg_clamp_hour(g_cfg.night_end);
  strlcpy(b.tz, g_cfg.tz, sizeof(b.tz));
  b.crc = cfg_crc32((const uint8_t *)&b, sizeof(b) - sizeof(b.crc));
  const size_t written = g_prefs.putBytes(CFG_BLOB_KEY, &b, sizeof(b));
  memset(&b, 0, sizeof(b));
  if (written != sizeof(ConfigBlob)) {
    LOGE("cfg", "could not write the configuration (%u of %u bytes)", (unsigned)written, (unsigned)sizeof(ConfigBlob));
    return false;
  }
  return true;
}

// Returns true when a valid record was read. Every version shares the same
// first fields, so one read covers them all: the stored length says which
// version it is, the fields beyond it get today's defaults, and anything older
// than the current version is written back once in the current format.
static bool config_read_blob() {
  union {  // the union keeps the bytes 4-byte aligned for the CRC read
    ConfigBlob v3;
    uint8_t raw[sizeof(ConfigBlob)];
  } u;
  memset(&u, 0, sizeof(u));

  const size_t len = g_prefs.getBytesLength(CFG_BLOB_KEY);
  if (len == 0) return false;
  uint16_t ver = 0;
  if (len == sizeof(ConfigBlob)) ver = 3;
  else if (len == sizeof(ConfigBlobV2)) ver = 2;
  else if (len == sizeof(ConfigBlobV1)) ver = 1;
  else {
    LOGW("cfg", "stored configuration has an unexpected size (%u B) - ignoring it", (unsigned)len);
    return false;
  }
  if (g_prefs.getBytes(CFG_BLOB_KEY, u.raw, sizeof(u.raw)) != len) return false;

  if (u.v3.magic != CFG_BLOB_MAGIC || u.v3.version != ver) {
    LOGW("cfg", "stored configuration is from another firmware version - ignoring it");
    return false;
  }
  uint32_t stored_crc = 0;
  memcpy(&stored_crc, u.raw + len - sizeof(stored_crc), sizeof(stored_crc));
  if (stored_crc != cfg_crc32(u.raw, len - sizeof(stored_crc))) {
    LOGE("cfg", "stored configuration failed its checksum - ignoring it (setup will start)");
    return false;
  }

  strlcpy(g_cfg.ssid, u.v3.ssid, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.pass, u.v3.pass, sizeof(g_cfg.pass));
  strlcpy(g_cfg.client_id, u.v3.client_id, sizeof(g_cfg.client_id));
  strlcpy(g_cfg.client_secret, u.v3.client_secret, sizeof(g_cfg.client_secret));
  strlcpy(g_cfg.auth_b64, u.v3.auth_b64, sizeof(g_cfg.auth_b64));
  strlcpy(g_cfg.refresh_token, u.v3.refresh_token, sizeof(g_cfg.refresh_token));
  if (ver >= 2) g_cfg.brightness = cfg_clamp_brightness(u.v3.brightness);
  if (ver >= 3) {
    g_cfg.night_on = u.v3.night_on ? 1 : 0;
    g_cfg.night_start = cfg_clamp_hour(u.v3.night_start);
    g_cfg.night_end = cfg_clamp_hour(u.v3.night_end);
    u.v3.tz[sizeof(u.v3.tz) - 1] = 0;
    strlcpy(g_cfg.tz, u.v3.tz, sizeof(g_cfg.tz));
  }
  memset(&u, 0, sizeof(u));
  if (ver != CFG_BLOB_VER) {
    LOGI("cfg", "stored configuration upgraded from version %u to %u", (unsigned)ver, (unsigned)CFG_BLOB_VER);
    config_store();
  }
  return true;
}

// Boards provisioned by the first version of this firmware kept one key per
// field. Fold them into the blob once, then drop them.
static bool config_migrate_legacy() {
  char ssid[sizeof(g_cfg.ssid)] = "";
  g_prefs.getString("ssid", ssid, sizeof(ssid));
  char auth[sizeof(g_cfg.auth_b64)] = "";
  g_prefs.getString("auth", auth, sizeof(auth));
  char rtok[sizeof(g_cfg.refresh_token)] = "";
  g_prefs.getString("rtok", rtok, sizeof(rtok));
  if (!ssid[0] && !auth[0] && !rtok[0]) return false;

  strlcpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.auth_b64, auth, sizeof(g_cfg.auth_b64));
  strlcpy(g_cfg.refresh_token, rtok, sizeof(g_cfg.refresh_token));
  g_prefs.getString("pass", g_cfg.pass, sizeof(g_cfg.pass));
  g_prefs.getString("cid", g_cfg.client_id, sizeof(g_cfg.client_id));
  g_prefs.getString("csec", g_cfg.client_secret, sizeof(g_cfg.client_secret));
  LOGI("cfg", "migrating the stored settings to the new single-record format");
  if (!config_store()) return false;
  for (const char *k : {"ssid", "pass", "cid", "csec", "auth", "rtok"}) g_prefs.remove(k);
  return true;
}

static void config_load() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  config_defaults();  // what a board with nothing stored uses
  g_prefs.begin(CFG_NAMESPACE, false);

  if (!config_read_blob()) config_migrate_legacy();
}

static void config_save_wifi(const char *ssid, const char *pass) {
  strlcpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.pass, pass, sizeof(g_cfg.pass));
  if (config_store()) LOGI("cfg", "saved Wi-Fi credentials for \"%s\"", g_cfg.ssid);
}

static bool config_save_client(const char *id, const char *secret) {
  char b64[sizeof(g_cfg.auth_b64)];
  if (!config_make_auth_b64(id, secret, b64, sizeof(b64))) {
    LOGE("cfg", "client id/secret too long to encode");
    return false;
  }
  strlcpy(g_cfg.client_id, id, sizeof(g_cfg.client_id));
  strlcpy(g_cfg.client_secret, secret, sizeof(g_cfg.client_secret));
  strlcpy(g_cfg.auth_b64, b64, sizeof(g_cfg.auth_b64));
  if (!config_store()) return false;
  LOGI("cfg", "saved Spotify client id %.6s... and secret (%u chars)", g_cfg.client_id, (unsigned)strlen(secret));
  return true;
}

static void config_save_token(const char *refresh_token) {
  strlcpy(g_cfg.refresh_token, refresh_token, sizeof(g_cfg.refresh_token));
  if (config_store())
    LOGI("cfg", "saved refresh token (%.6s..., %u chars)", refresh_token, (unsigned)strlen(refresh_token));
}

// Called when the brightness slider is released, not while it moves: one flash
// write per adjustment instead of one per step.
static void config_save_brightness(uint8_t percent) {
  const uint8_t v = cfg_clamp_brightness(percent);
  if (v == g_cfg.brightness) return;
  g_cfg.brightness = v;
  if (config_store()) LOGI("cfg", "saved screen brightness %u%%", (unsigned)v);
}

static void config_save_night(bool on, int start_h, int end_h, const char *tz) {
  g_cfg.night_on = on ? 1 : 0;
  g_cfg.night_start = cfg_clamp_hour(start_h);
  g_cfg.night_end = cfg_clamp_hour(end_h);
  if (tz && tz[0]) strlcpy(g_cfg.tz, tz, sizeof(g_cfg.tz));
  if (config_store())
    LOGI("cfg", "saved night mode: %s, %02u:00 to %02u:00, time zone %s", g_cfg.night_on ? "on" : "off",
         (unsigned)g_cfg.night_start, (unsigned)g_cfg.night_end, g_cfg.tz);
}

static void config_clear_token() {
  g_cfg.refresh_token[0] = 0;
  config_store();
  LOGW("cfg", "cleared the stored refresh token - re-authorization needed");
}

// Wipes everything, so the next boot behaves like a freshly flashed device.
static void config_forget() {
  g_prefs.clear();
  memset(&g_cfg, 0, sizeof(g_cfg));
  config_defaults();  // brightness and night mode go too - back to out-of-the-box values
  LOGW("cfg", "all stored settings erased");
}

static void config_log() {
  char night[80] = "off";
  if (g_cfg.night_set())
    snprintf(night, sizeof(night), "%02u:00-%02u:00 %s", (unsigned)g_cfg.night_start, (unsigned)g_cfg.night_end, g_cfg.tz);
  LOGI("cfg", "Wi-Fi \"%s\" %s | client %s | refresh token %s | brightness %u%% | night %s", g_cfg.ssid,
       g_cfg.has_wifi() ? "set" : "MISSING", g_cfg.has_client() ? "set" : "MISSING",
       g_cfg.has_token() ? "set" : "MISSING", (unsigned)g_cfg.brightness, night);
}

#endif  // _APP_CONFIG_H_
