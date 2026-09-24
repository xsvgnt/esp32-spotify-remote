#ifndef _SETUP_PORTAL_H_
#define _SETUP_PORTAL_H_

/*
 * setup_portal.h - on-device provisioning, so a flashed board needs no
 * compiled-in secrets.
 *
 * Two shapes, same pages:
 *   PORTAL_AP   no Wi-Fi configured (or you asked for setup while offline):
 *               the board opens a WPA2 access point and a captive portal at
 *               http://192.168.4.1 . The screen shows the network name, its
 *               password and a Wi-Fi QR code.
 *   PORTAL_LAN  Wi-Fi works: the pages are served on the board's LAN address,
 *               which the screen shows as text and as a QR code.
 *
 * Authorization (paste-back, so the redirect URI stays the loopback one that
 * the PC helper uses and nothing extra has to be hosted):
 *   1. the page sends the phone to accounts.spotify.com in a new tab
 *   2. after "Agree", Spotify redirects to http://127.0.0.1:8888/callback,
 *      which cannot load - that is expected, the address bar is what matters
 *   3. the address is pasted back into the page; the board pulls "code" out of
 *      it, exchanges it for a refresh token and stores that in NVS.
 *
 * Everything here runs on net_task (core 0) and never calls LVGL: the screen
 * reads the strings below through view_get().
 */

#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include "app_config.h"
#include "spotify_api.h"

// (the redirect URI itself lives in spotify_api.h, next to the rest of the OAuth config)
#define PORTAL_AP_PREFIX    "SpotifyRemote-"
#define PORTAL_HTTP_PORT    80
#define PORTAL_DNS_PORT     53
#define WIFI_TEST_TIMEOUT_MS 15000
#define PORTAL_IDLE_EXIT_MS  180000  // nothing left to set up and nobody using the page -> back to the player

enum PortalMode : uint8_t { PORTAL_OFF = 0, PORTAL_AP, PORTAL_LAN };

// ---------------------------------------------------------------------------
// What the screen shows while the portal is up (written here, read by the UI)
// ---------------------------------------------------------------------------
struct SetupView {
  char title[32];
  char line1[64];
  char line2[72];
  char line3[72];
  char line4[96];  // status line: what the portal is doing right now
  char qr[192];
  uint32_t seq;
};

static SetupView g_view;
static SemaphoreHandle_t g_view_mtx = nullptr;

static void view_set(const char *title, const char *l1, const char *l2, const char *l3, const char *l4, const char *qr) {
  if (!g_view_mtx) g_view_mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(g_view_mtx, portMAX_DELAY);
  strlcpy(g_view.title, title ? title : "", sizeof(g_view.title));
  strlcpy(g_view.line1, l1 ? l1 : "", sizeof(g_view.line1));
  strlcpy(g_view.line2, l2 ? l2 : "", sizeof(g_view.line2));
  strlcpy(g_view.line3, l3 ? l3 : "", sizeof(g_view.line3));
  strlcpy(g_view.line4, l4 ? l4 : "", sizeof(g_view.line4));
  strlcpy(g_view.qr, qr ? qr : "", sizeof(g_view.qr));
  g_view.seq++;
  xSemaphoreGive(g_view_mtx);
}

// Bottom line of the setup screen: what the portal is doing right now.
static void view_set_status(const char *l4) {
  if (!g_view_mtx) g_view_mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(g_view_mtx, portMAX_DELAY);
  strlcpy(g_view.line4, l4 ? l4 : "", sizeof(g_view.line4));
  g_view.seq++;
  xSemaphoreGive(g_view_mtx);
}

static void view_get(SetupView &out) {
  if (!g_view_mtx) g_view_mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(g_view_mtx, portMAX_DELAY);
  out = g_view;
  xSemaphoreGive(g_view_mtx);
}

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *g_http = nullptr;
static DNSServer *g_dns = nullptr;
static PortalMode g_portal_mode = PORTAL_OFF;
static char g_ap_ssid[33] = "";
static char g_ap_pass[16] = "";
static char g_oauth_state[20] = "";
static char g_portal_note[120] = "";   // last action, shown on the page
static uint32_t g_portal_last_req = 0;          // for the idle timeout
static volatile bool g_portal_auth_ok = false;  // a refresh token was just obtained

static uint32_t portal_idle_ms() { return millis() - g_portal_last_req; }

static String portal_base_url() {
  return String("http://") + (g_portal_mode == PORTAL_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------
static const char PORTAL_CSS[] PROGMEM =
  "<meta name=viewport content='width=device-width,initial-scale=1'><meta charset=utf-8>"
  "<style>"
  "*{box-sizing:border-box}"
  "body{margin:0;padding:18px;background:#121212;color:#fff;font:16px/1.5 -apple-system,Roboto,Segoe UI,sans-serif}"
  "h1{font-size:20px;margin:4px 0 16px}h2{font-size:16px;margin:0 0 10px;color:#1DB954}"
  ".c{background:#1c1c1c;border:1px solid #2c2c2c;border-radius:12px;padding:14px;margin:0 0 14px}"
  "label{display:block;margin:10px 0 4px;font-size:14px;color:#b3b3b3}"
  "input,select{width:100%;padding:12px;border-radius:8px;border:1px solid #3a3a3a;background:#101010;color:#fff;font-size:16px}"
  "button,.btn{display:inline-block;width:100%;margin-top:14px;padding:13px;border:0;border-radius:24px;background:#1DB954;"
  "color:#000;font-size:16px;font-weight:600;text-align:center;text-decoration:none;cursor:pointer}"
  ".btn2{background:#2c2c2c;color:#fff}.danger{background:#7a1f1f;color:#fff}"
  "code{background:#000;padding:3px 6px;border-radius:6px;font-size:13px;word-break:break-all}"
  ".nb{white-space:nowrap}"
  "p{margin:8px 0;font-size:14px;color:#b3b3b3}.ok{color:#1DB954}.bad{color:#ff6b6b}"
  "ol{margin:8px 0 0 18px;padding:0;font-size:14px;color:#b3b3b3}li{margin:6px 0}"
  "</style>";

static void html_escape(const char *in, String &out) {
  for (const char *p = in; *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p;
    }
  }
}

static void portal_send_page(const String &body, const char *heading = "Spotify Remote setup") {
  g_portal_last_req = millis();
  String p;
  p.reserve(body.length() + sizeof(PORTAL_CSS) + 256);
  p += F("<!doctype html><html><head><title>Spotify Remote</title>");
  p += FPSTR(PORTAL_CSS);
  p += F("</head><body><h1>");
  p += heading;
  p += F("</h1>");
  if (g_portal_note[0]) {
    p += F("<div class=c><p class=ok>");
    html_escape(g_portal_note, p);
    p += F("</p></div>");
  }
  p += body;
  p += F("</body></html>");
  g_http->send(200, "text/html; charset=utf-8", p);
}

static String portal_ssid_options() {
  String s;
  const int n = WiFi.scanComplete();
  if (n <= 0) return s;
  s += F("<datalist id=nets>");
  for (int i = 0; i < n && i < 20; ++i) {
    s += F("<option value='");
    html_escape(WiFi.SSID(i).c_str(), s);
    s += F("'>");
  }
  s += F("</datalist>");
  return s;
}

static void handle_root() {
  const bool sta_up = WiFi.status() == WL_CONNECTED;
  String b;
  b.reserve(4096);

  // --- status ---
  b += F("<div class=c><h2>Status</h2><p>Wi-Fi: ");
  if (sta_up) {
    b += F("<span class=ok>connected to ");
    html_escape(g_cfg.ssid, b);
    b += F("</span> (");
    b += WiFi.localIP().toString();
    b += ')';
  } else {
    b += F("<span class=bad>not connected</span>");
  }
  b += F("</p><p>Spotify app: ");
  b += g_cfg.has_client() ? F("<span class=ok>saved</span>") : F("<span class=bad>not set</span>");
  b += F("</p><p>Authorization: ");
  b += g_cfg.has_token() ? F("<span class=ok>done</span>") : F("<span class=bad>missing</span>");
  b += F("</p></div>");

  // --- step 1: Wi-Fi ---
  // Each step is its own form with its own button: one step is saved at a time,
  // in order. (A <form> must stay inside one <div> - browsers drop the fields of
  // a form that crosses a closing tag.)
  b += F("<div class=c><h2>1. Wi-Fi</h2><form method=POST action=/save>");
  b += portal_ssid_options();
  b += F("<label>Network name</label><input name=ssid list=nets autocapitalize=off autocorrect=off value='");
  html_escape(g_cfg.ssid, b);
  b += F("'><label>Password");
  if (g_cfg.has_wifi()) b += F(" (leave empty to keep the saved one)");
  b += F("</label><input name=wpass type=password autocomplete=off>"
         "<button type=submit>Save Wi-Fi</button></form></div>");

  // --- step 2: Spotify app ---
  b += F("<div class=c><h2>2. Spotify app</h2><p>In the Spotify developer dashboard, create an app and add "
         "this exact redirect URI:</p><p><code>" SPOTIFY_REDIRECT_URI "</code></p>"
         "<form method=POST action=/save>"
         "<label>Client ID</label><input name=cid autocapitalize=off autocorrect=off value='");
  html_escape(g_cfg.client_id, b);
  b += F("'><label>Client secret");
  if (g_cfg.has_client()) b += F(" (leave empty to keep the saved one)");
  b += F("</label><input name=csec type=password autocomplete=off>"
         "<button type=submit>Save Spotify app</button></form></div>");

  // --- step 3: authorize ---
  b += F("<div class=c><h2>3. Authorize</h2>");
  if (!sta_up) {
    b += F("<p>Save the Wi-Fi settings and restart first: this step needs the board to be online.</p>");
  } else if (g_portal_mode == PORTAL_AP) {
    // The phone is on the board's own network, which has no way out to the
    // internet - and the captive-portal DNS answers every name with the board.
    b += F("<p>Your phone is on the board's setup network, which has no internet access. "
           "Restart the board so it joins your Wi-Fi, then reopen this page at the address shown on its "
           "screen to finish this step.</p>"
           "<form method=POST action=/restart><button type=submit>Restart now</button></form>");
  } else if (!g_cfg.has_client()) {
    b += F("<p>Save your client ID and secret first.</p>");
  } else {
    b += F("<ol><li>Open the Spotify page (new tab) and tap <b>Agree</b>.</li>"
           "<li>The browser then fails to open <code class=nb>127.0.0.1:8888</code> - that is expected.</li>"
           "<li>Copy the whole address from the address bar, come back here and paste it below.</li></ol>"
           "<a class='btn btn2' href=/auth target=_blank rel=noopener>Open Spotify authorization</a>"
           "<form method=POST action=/paste><label>Pasted address (or just the code)</label>"
           "<input name=url placeholder='http://127.0.0.1:8888/callback?code=...'>"
           "<button type=submit>Finish setup</button></form>");
  }
  b += F("</div>");

  // --- maintenance ---
  b += F("<div class=c><h2>Device</h2><form method=POST action=/restart><button class='btn btn2' type=submit>Restart</button></form>"
         "<form method=POST action=/forget onsubmit=\"return confirm('Erase Wi-Fi and Spotify credentials?')\">"
         "<button class='btn danger' type=submit>Forget everything</button></form></div>");
  portal_send_page(b);
}

// Tries the credentials before storing them, so a typo cannot lock you out.
static bool portal_test_wifi(const char *ssid, const char *pass, char *err, size_t errn) {
  LOGI("portal", "testing Wi-Fi credentials for \"%s\"", ssid);
  view_set_status("Testing Wi-Fi...");
  WiFi.mode(g_portal_mode == PORTAL_AP ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid, pass);
  const uint32_t t0 = millis();
  while (millis() - t0 < WIFI_TEST_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      LOGI("portal", "Wi-Fi test OK, IP %s (%lu ms)", WiFi.localIP().toString().c_str(), (unsigned long)(millis() - t0));
      return true;
    }
    delay(100);
  }
  snprintf(err, errn, "Could not connect to \"%s\" - check the name and password", ssid);
  LOGW("portal", "Wi-Fi test failed for \"%s\"", ssid);
  WiFi.disconnect();
  return false;
}

static void handle_save() {
  String ssid = g_http->arg("ssid");
  String wpass = g_http->arg("wpass");
  String cid = g_http->arg("cid");
  String csec = g_http->arg("csec");
  ssid.trim();
  cid.trim();
  csec.trim();
  char err[120] = "";
  g_portal_note[0] = 0;

  // Each step has its own button, so a submission carries one step's fields;
  // whatever is missing is simply left as it is.
  bool client_saved = false;
  if (cid.length() && csec.length()) {
    if (config_save_client(cid.c_str(), csec.c_str())) client_saved = true;
    else strlcpy(err, "Client id/secret too long", sizeof(err));
  } else if (cid.length() && !csec.length() && (!g_cfg.has_client() || cid != g_cfg.client_id)) {
    strlcpy(err, "Enter the client secret as well: it is stored together with the client ID", sizeof(err));
  } else if (csec.length() && !cid.length()) {
    strlcpy(err, "Enter the client ID as well", sizeof(err));
  }

  bool wifi_changed = false, wifi_saved = false;
  if (!err[0] && ssid.length()) {
    const String pass = wpass.length() ? wpass : String(g_cfg.pass);
    const bool same = (ssid == g_cfg.ssid) && !wpass.length();
    wifi_changed = !same;
    // Testing means joining the new network, which would cut off this very page
    // when it is served over the old one - so only test from the access point.
    const bool test_ok = same || g_portal_mode != PORTAL_AP || portal_test_wifi(ssid.c_str(), pass.c_str(), err, sizeof(err));
    if (test_ok && !err[0]) {
      config_save_wifi(ssid.c_str(), pass.c_str());
      wifi_saved = true;
    }
  } else if (!err[0] && wpass.length()) {
    strlcpy(err, "Enter the network name as well", sizeof(err));
  }

  String b;
  if (err[0]) {
    b += F("<div class=c><p class=bad>");
    html_escape(err, b);
    b += F("</p><a class='btn btn2' href=/>Back</a></div>");
    view_set_status("Check the setup page");
  } else if (!wifi_saved && !client_saved) {
    b += F("<div class=c><p>Nothing to save - the fields of that step were empty.</p>"
           "<a class='btn btn2' href=/>Back</a></div>");
  } else {
    snprintf(g_portal_note, sizeof(g_portal_note), "%s saved.",
             wifi_saved && client_saved ? "Wi-Fi and Spotify app" : (wifi_saved ? "Wi-Fi" : "Spotify app"));
    b += F("<div class=c><p class=ok>Saved.</p>");
    if (wifi_changed && g_portal_mode == PORTAL_AP) {
      // Still on the board's own network: the remaining steps can be filled in
      // from here, but the authorization needs a phone with internet access.
      b += F("<p>Next: fill in your Spotify app (step 2). Then restart the board so it joins your network and "
             "reopen this page at the address shown on its screen to authorize it.</p>"
             "<a class=btn href=/>Continue setup</a>"
             "<form method=POST action=/restart><button class='btn btn2' type=submit>Restart now</button></form>");
      view_set_status("Wi-Fi saved - continue on the page");
    } else if (wifi_changed) {
      b += F("<p>The new Wi-Fi settings apply after a restart. The board then shows its new address on the screen.</p>"
             "<a class=btn href=/>Continue</a>"
             "<form method=POST action=/restart><button class='btn btn2' type=submit>Restart now</button></form>");
      view_set_status("Saved - restart to apply");
    } else {
      b += F("<a class=btn href=/>Continue</a>");
      view_set_status(client_saved && !wifi_saved ? "Spotify app saved" : "Settings saved");
    }
    b += F("</div>");
  }
  portal_send_page(b, err[0] ? "Not saved" : (wifi_saved || client_saved) ? "Saved" : "Nothing to save");
}

// Sends the phone to Spotify's consent page.
static void handle_auth() {
  if (!g_cfg.has_client()) {
    g_http->send(400, "text/plain", "No client id stored yet");
    return;
  }
  g_portal_last_req = millis();
  snprintf(g_oauth_state, sizeof(g_oauth_state), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  String url = F("https://accounts.spotify.com/authorize?response_type=code&client_id=");
  url += g_cfg.client_id;
  url += F("&scope=");
  url += url_encode(SPOTIFY_REQUIRED_SCOPES);
  url += F("&redirect_uri=");
  url += url_encode(SPOTIFY_REDIRECT_URI);
  url += F("&state=");
  url += g_oauth_state;
  url += F("&show_dialog=true");
  LOGI("portal", "sending the browser to the Spotify consent page (state %s)", g_oauth_state);
  view_set_status("Waiting for authorization...");
  g_http->sendHeader("Location", url);
  g_http->send(302, "text/plain", "");
}

// Pulls "code" (and "state") out of whatever was pasted back.
static bool parse_pasted_redirect(const char *in, char *code, size_t code_n, char *state, size_t state_n, char *err,
                                  size_t err_n) {
  code[0] = 0;
  state[0] = 0;
  err[0] = 0;
  while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') ++in;
  if (!*in) {
    snprintf(err, err_n, "Nothing pasted");
    return false;
  }
  const char *e = strstr(in, "error=");
  const char *c = strstr(in, "code=");
  if (e && (!c || e < c)) {
    size_t n = strcspn(e + 6, "&# \t\r\n");
    snprintf(err, err_n, "Spotify returned an error: %.*s", (int)n, e + 6);
    return false;
  }
  if (c) {
    const char *v = c + 5;
    const size_t n = strcspn(v, "&# \t\r\n");
    if (n == 0 || n >= code_n) {
      snprintf(err, err_n, "The pasted address has no usable code");
      return false;
    }
    memcpy(code, v, n);
    code[n] = 0;
    const char *s = strstr(in, "state=");
    if (s) {
      const size_t sn = strcspn(s + 6, "&# \t\r\n");
      if (sn && sn < state_n) {
        memcpy(state, s + 6, sn);
        state[sn] = 0;
      }
    }
    return true;
  }
  // Not a URL: assume the bare code was pasted.
  const size_t n = strcspn(in, " \t\r\n");
  if (n < 20 || n >= code_n) {
    snprintf(err, err_n, "That does not look like the address Spotify redirected to");
    return false;
  }
  memcpy(code, in, n);
  code[n] = 0;
  return true;
}

static void handle_paste() {
  const String pasted = g_http->arg("url");
  char code[512], state[40], err[160];
  String b;
  bool ok = parse_pasted_redirect(pasted.c_str(), code, sizeof(code), state, sizeof(state), err, sizeof(err));

  if (ok && !g_oauth_state[0]) {  // nothing in flight: a stale or replayed address
    ok = false;
    snprintf(err, sizeof(err), "Open the Spotify authorization first (step 3), then paste the address it sends you to");
  }
  if (ok && state[0] && strcmp(state, g_oauth_state) != 0) {
    ok = false;
    snprintf(err, sizeof(err), "This address belongs to a different authorization attempt - start again");
  }
  if (ok) {
    LOGI("portal", "exchanging the authorization code for a refresh token");
    view_set_status("Finishing authorization...");
    ok = spotify_exchange_code(code, err, sizeof(err));
  }
  memset(code, 0, sizeof(code));

  if (ok) {
    g_portal_auth_ok = true;
    g_oauth_state[0] = 0;
    snprintf(g_portal_note, sizeof(g_portal_note), "Connected to Spotify.");
    b += F("<div class=c><p class=ok>Done - the board is connected to your Spotify account.</p>"
           "<p>You can close this page. Playback appears on the screen within a few seconds.</p></div>");
    view_set_status("Connected");
  } else {
    LOGW("portal", "authorization failed: %s", err);
    b += F("<div class=c><p class=bad>");
    html_escape(err, b);
    b += F("</p><p>Open the Spotify authorization again and paste the new address.</p>"
           "<a class='btn btn2' href=/>Back</a></div>");
    view_set_status("Authorization failed");
  }
  portal_send_page(b, ok ? "Connected" : "Not connected");
}

static void handle_restart() {
  portal_send_page(F("<div class=c><p>Restarting. Reopen this page at the address shown on the screen.</p></div>"),
                   "Restarting");
  view_set("Restarting", "", "", "", "", "");
  LOGW("portal", "restart requested from the setup page");
  delay(600);
  ESP.restart();
}

static void handle_forget() {
  config_forget();
  portal_send_page(F("<div class=c><p>All settings erased. The board restarts and opens its setup network again.</p></div>"),
                   "Erased");
  delay(600);
  ESP.restart();
}

static void handle_not_found() {
  if (g_portal_mode == PORTAL_AP) {  // captive portal: send everything to the setup page
    g_http->sendHeader("Location", portal_base_url() + "/");
    g_http->send(302, "text/plain", "");
    return;
  }
  g_http->send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------------------
// Start / stop / service
// ---------------------------------------------------------------------------
static void portal_make_ap_credentials() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_ap_ssid, sizeof(g_ap_ssid), PORTAL_AP_PREFIX "%02X%02X", mac[4], mac[5]);
  snprintf(g_ap_pass, sizeof(g_ap_pass), "sp%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);  // 10 chars, stable
}

static void portal_start(PortalMode mode) {
  if (g_portal_mode == mode) return;
  g_portal_mode = mode;
  g_portal_note[0] = 0;
  g_portal_last_req = millis();
  api_close();

  if (mode == PORTAL_AP) {
    portal_make_ap_credentials();
    WiFi.mode(WIFI_AP_STA);  // AP_STA so the page can scan and test networks
    WiFi.softAP(g_ap_ssid, g_ap_pass);
    delay(200);
    const String ip = WiFi.softAPIP().toString();
    LOGI("portal", "access point \"%s\" up, password %s, page at http://%s", g_ap_ssid, g_ap_pass, ip.c_str());
    char l3[72], l4[96];
    snprintf(l3, sizeof(l3), "Password %s", g_ap_pass);
    snprintf(l4, sizeof(l4), "Then open http://%s", ip.c_str());
    if (!g_dns) g_dns = new DNSServer();
    g_dns->setErrorReplyCode(DNSReplyCode::NoError);
    g_dns->start(PORTAL_DNS_PORT, "*", WiFi.softAPIP());  // captive portal
    WiFi.scanNetworks(true, true);                        // async, for the SSID list
    char qr[192];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", g_ap_ssid, g_ap_pass);  // the QR joins the network
    view_set("Setup", "Join this Wi-Fi network", g_ap_ssid, l3, l4, qr);
  } else {
    LOGI("portal", "setup page at http://%s", WiFi.localIP().toString().c_str());
    char l2[72];
    snprintf(l2, sizeof(l2), "http://%s", WiFi.localIP().toString().c_str());
    view_set(g_cfg.has_token() ? "Setup" : "Connect Spotify", "Open on your phone", l2,
             g_cfg.has_token() ? "" : "Then follow steps 1-3", "", l2);
  }

  if (!g_http) {
    g_http = new WebServer(PORTAL_HTTP_PORT);
    g_http->on("/", handle_root);
    g_http->on("/save", HTTP_POST, handle_save);
    g_http->on("/auth", handle_auth);
    g_http->on("/paste", HTTP_POST, handle_paste);
    g_http->on("/restart", HTTP_POST, handle_restart);
    g_http->on("/forget", HTTP_POST, handle_forget);
    g_http->onNotFound(handle_not_found);
  }
  g_http->begin();
  log_heap("portal up");
}

static void portal_stop() {
  if (g_portal_mode == PORTAL_OFF) return;
  LOGI("portal", "stopping the setup portal");
  if (g_http) {
    g_http->stop();
    delete g_http;
    g_http = nullptr;
  }
  if (g_dns) {
    g_dns->stop();
    delete g_dns;
    g_dns = nullptr;
  }
  if (g_portal_mode == PORTAL_AP) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
  WiFi.scanDelete();
  g_portal_mode = PORTAL_OFF;
  log_heap("portal down");
}

static void portal_loop() {
  if (!g_http) return;
  if (g_dns) g_dns->processNextRequest();
  g_http->handleClient();
}

#endif  // _SETUP_PORTAL_H_
