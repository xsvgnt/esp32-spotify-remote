#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

/*
 * app_config.h - credentials kept in NVS (flash), not compiled into the sketch.
 *
 * Stored: Wi-Fi SSID/password, Spotify client id/secret (plus the base64 of
 * "id:secret", computed once here so requests never do it), and the refresh
 * token obtained by the on-device authorization.
 *
 * Everything lives in ONE NVS blob with a magic number, a version and a CRC.
 * NVS writes a new entry before invalidating the old one, so a single-key write
 * is atomic: a power cut leaves either the previous complete record or the new
 * one, never a mixture (which several separate keys could produce - Wi-Fi name
 * stored without its password, say). A record that fails its CRC is ignored,
 * and the board starts its setup portal.
 *
 * NVS is NOT encrypted: anyone with a USB cable and esptool can read this out
 * of flash unless you enable flash encryption (an irreversible efuse change).
 * The Wi-Fi password is in NVS anyway - the Wi-Fi stack stores its own copy.
 *
 * secrets.h is only a seed: on a device with empty NVS its values (when they
 * are not the "XXX" placeholders) are copied in once, so an already-working
 * device keeps working after the update. "Forget everything" in the setup page
 * clears NVS and disables the seed, giving a true out-of-the-box first boot.
 * Set CONFIG_SEED_FROM_SECRETS to 0 to ignore secrets.h completely.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include "secrets.h"

#ifndef CONFIG_SEED_FROM_SECRETS
#define CONFIG_SEED_FROM_SECRETS 1
#endif

#define CFG_NAMESPACE  "spotcfg"
#define CFG_BLOB_KEY   "cfg"
#define CFG_BLOB_MAGIC 0x53504F54UL  // "SPOT"
#define CFG_BLOB_VER   1

struct AppConfig {
  char ssid[33];
  char pass[65];
  char client_id[64];
  char client_secret[96];
  char auth_b64[224];      // base64("client_id:client_secret")
  char refresh_token[320];

  bool has_wifi() const { return ssid[0] != 0; }
  bool has_client() const { return client_id[0] != 0 && auth_b64[0] != 0; }
  bool has_token() const { return refresh_token[0] != 0; }
};

// On-flash layout: packed and versioned, so it does not depend on how the
// compiler happens to lay out AppConfig.
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
  uint32_t crc;  // over every byte before this field
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

static bool config_is_placeholder(const char *v) {
  return v == nullptr || v[0] == 0 || strcmp(v, "XXX") == 0;
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
  b.crc = cfg_crc32((const uint8_t *)&b, sizeof(b) - sizeof(b.crc));
  const size_t written = g_prefs.putBytes(CFG_BLOB_KEY, &b, sizeof(b));
  memset(&b, 0, sizeof(b));
  if (written != sizeof(ConfigBlob)) {
    LOGE("cfg", "could not write the configuration (%u of %u bytes)", (unsigned)written, (unsigned)sizeof(ConfigBlob));
    return false;
  }
  return true;
}

// Returns true when a valid record was read.
static bool config_read_blob() {
  ConfigBlob b;
  const size_t len = g_prefs.getBytes(CFG_BLOB_KEY, &b, sizeof(b));
  if (len == 0) return false;
  if (len != sizeof(ConfigBlob)) {
    LOGW("cfg", "stored configuration has an unexpected size (%u B) - ignoring it", (unsigned)len);
    return false;
  }
  if (b.magic != CFG_BLOB_MAGIC || b.version != CFG_BLOB_VER) {
    LOGW("cfg", "stored configuration is from another firmware version - ignoring it");
    return false;
  }
  if (b.crc != cfg_crc32((const uint8_t *)&b, sizeof(b) - sizeof(b.crc))) {
    LOGE("cfg", "stored configuration failed its checksum - ignoring it (setup will start)");
    return false;
  }
  strlcpy(g_cfg.ssid, b.ssid, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.pass, b.pass, sizeof(g_cfg.pass));
  strlcpy(g_cfg.client_id, b.client_id, sizeof(g_cfg.client_id));
  strlcpy(g_cfg.client_secret, b.client_secret, sizeof(g_cfg.client_secret));
  strlcpy(g_cfg.auth_b64, b.auth_b64, sizeof(g_cfg.auth_b64));
  strlcpy(g_cfg.refresh_token, b.refresh_token, sizeof(g_cfg.refresh_token));
  memset(&b, 0, sizeof(b));
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
  g_prefs.begin(CFG_NAMESPACE, false);

  if (!config_read_blob()) config_migrate_legacy();

#if CONFIG_SEED_FROM_SECRETS
  if (!g_prefs.getBool("seeded", false)) {
    g_prefs.putBool("seeded", true);
    bool seeded = false;
    if (!g_cfg.has_wifi() && !config_is_placeholder(SSID)) {
      strlcpy(g_cfg.ssid, SSID, sizeof(g_cfg.ssid));
      strlcpy(g_cfg.pass, PASSWORD, sizeof(g_cfg.pass));
      seeded = true;
    }
    if (!g_cfg.has_client() && !config_is_placeholder(AUTH_B64)) {
      strlcpy(g_cfg.auth_b64, AUTH_B64, sizeof(g_cfg.auth_b64));
      strlcpy(g_cfg.client_id, "(from secrets.h)", sizeof(g_cfg.client_id));
      seeded = true;
    }
    if (!g_cfg.has_token() && !config_is_placeholder(REFRESH_TOKEN)) {
      strlcpy(g_cfg.refresh_token, REFRESH_TOKEN, sizeof(g_cfg.refresh_token));
      seeded = true;
    }
    if (seeded) {
      LOGI("cfg", "seeded the stored configuration from secrets.h (first boot only)");
      config_store();
    }
  }
#endif
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

static void config_clear_token() {
  g_cfg.refresh_token[0] = 0;
  config_store();
  LOGW("cfg", "cleared the stored refresh token - re-authorization needed");
}

// Wipes everything and keeps the secrets.h seed disabled, so the next boot
// behaves like a freshly flashed device.
static void config_forget() {
  g_prefs.clear();
  g_prefs.putBool("seeded", true);
  memset(&g_cfg, 0, sizeof(g_cfg));
  LOGW("cfg", "all stored settings erased");
}

static void config_log() {
  LOGI("cfg", "Wi-Fi \"%s\" %s | client %s | refresh token %s", g_cfg.ssid, g_cfg.has_wifi() ? "set" : "MISSING",
       g_cfg.has_client() ? "set" : "MISSING", g_cfg.has_token() ? "set" : "MISSING");
}

// secrets.h is done with: drop its macros so they cannot collide with real
// identifiers later (WiFi.SSID() is a method, "SSID" as a macro breaks it).
#undef SSID
#undef PASSWORD
#undef AUTH_B64
#undef REFRESH_TOKEN

#endif  // _APP_CONFIG_H_
