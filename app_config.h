#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

/*
 * app_config.h - credentials kept in NVS (flash), not compiled into the sketch.
 *
 * Stored: Wi-Fi SSID/password, Spotify client id/secret (plus the base64 of
 * "id:secret", computed once here so requests never do it), and the refresh
 * token obtained by the on-device authorization.
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

#define CFG_NAMESPACE "spotcfg"

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

static AppConfig g_cfg;
static Preferences g_prefs;

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

static void config_load() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_prefs.begin(CFG_NAMESPACE, false);
  g_prefs.getString("ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
  g_prefs.getString("pass", g_cfg.pass, sizeof(g_cfg.pass));
  g_prefs.getString("cid", g_cfg.client_id, sizeof(g_cfg.client_id));
  g_prefs.getString("csec", g_cfg.client_secret, sizeof(g_cfg.client_secret));
  g_prefs.getString("auth", g_cfg.auth_b64, sizeof(g_cfg.auth_b64));
  g_prefs.getString("rtok", g_cfg.refresh_token, sizeof(g_cfg.refresh_token));

#if CONFIG_SEED_FROM_SECRETS
  if (!g_prefs.getBool("seeded", false)) {
    g_prefs.putBool("seeded", true);
    bool seeded = false;
    if (!g_cfg.has_wifi() && !config_is_placeholder(SSID)) {
      strlcpy(g_cfg.ssid, SSID, sizeof(g_cfg.ssid));
      strlcpy(g_cfg.pass, PASSWORD, sizeof(g_cfg.pass));
      g_prefs.putString("ssid", g_cfg.ssid);
      g_prefs.putString("pass", g_cfg.pass);
      seeded = true;
    }
    if (!g_cfg.has_client() && !config_is_placeholder(AUTH_B64)) {
      strlcpy(g_cfg.auth_b64, AUTH_B64, sizeof(g_cfg.auth_b64));
      strlcpy(g_cfg.client_id, "(from secrets.h)", sizeof(g_cfg.client_id));
      g_prefs.putString("auth", g_cfg.auth_b64);
      g_prefs.putString("cid", g_cfg.client_id);
      seeded = true;
    }
    if (!g_cfg.has_token() && !config_is_placeholder(REFRESH_TOKEN)) {
      strlcpy(g_cfg.refresh_token, REFRESH_TOKEN, sizeof(g_cfg.refresh_token));
      g_prefs.putString("rtok", g_cfg.refresh_token);
      seeded = true;
    }
    if (seeded) LOGI("cfg", "seeded the stored configuration from secrets.h (first boot only)");
  }
#endif
}

static void config_save_wifi(const char *ssid, const char *pass) {
  strlcpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid));
  strlcpy(g_cfg.pass, pass, sizeof(g_cfg.pass));
  g_prefs.putString("ssid", g_cfg.ssid);
  g_prefs.putString("pass", g_cfg.pass);
  LOGI("cfg", "saved Wi-Fi credentials for \"%s\"", g_cfg.ssid);
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
  g_prefs.putString("cid", g_cfg.client_id);
  g_prefs.putString("csec", g_cfg.client_secret);
  g_prefs.putString("auth", g_cfg.auth_b64);
  LOGI("cfg", "saved Spotify client id %.6s... and secret (%u chars)", g_cfg.client_id, (unsigned)strlen(secret));
  return true;
}

static void config_save_token(const char *refresh_token) {
  strlcpy(g_cfg.refresh_token, refresh_token, sizeof(g_cfg.refresh_token));
  g_prefs.putString("rtok", g_cfg.refresh_token);
  LOGI("cfg", "saved refresh token (%.6s..., %u chars)", refresh_token, (unsigned)strlen(refresh_token));
}

static void config_clear_token() {
  g_cfg.refresh_token[0] = 0;
  g_prefs.remove("rtok");
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
