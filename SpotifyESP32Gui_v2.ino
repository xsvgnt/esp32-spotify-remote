/*
 * SpotifyESP32Gui_v2 - Spotify "now playing" remote for a 1.8" round 360x360
 * ST77916 touchscreen (ESP32-S3, JC3636W518EN class). Talks to the Spotify
 * Web API directly over HTTPS; no Spotify library.
 *
 * Tools menu: board "ESP32S3 Dev Module", PSRAM enabled, Flash Mode QIO 80 MHz,
 * Flash Size 16 MB, Partition Scheme with an 8 MB app partition
 * (extras/partitions.csv if you need one), Arduino Runs On: Core 1.
 * Libraries: ESP32_Display_Panel 0.1.4, ESP32_IO_Expander 0.0.2, lvgl 8.4.0,
 * ArduinoJson 7.x, JPEGDEC. lv_conf.h (extras/) goes in libraries/, next to lvgl/.
 * Credentials: run extras/get_refresh_token.py once on a PC, then fill secrets.h.
 */

// Serial logging: 0 = off, 1 = errors, 2 = + warnings, 3 = + info, 4 = + debug
#define APP_LOG_LEVEL 3

#include "spotify_api.h"  // logging, diagnostics, Wi-Fi, tokens, spotify_request()
#include "spotify_ui.h"   // LVGL UI, artwork pipeline, net_task

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);  // USB-CDC: give the host a moment so boot logs are kept

  print_boot_diagnostics();
  ui_init();    // core 1: panel, LVGL, widgets, 100 ms UI timer
  net_start();  // core 0: Wi-Fi, Spotify API, artwork
}

void loop() {
  lv_timer_handler();
  vTaskDelay(5);
}
