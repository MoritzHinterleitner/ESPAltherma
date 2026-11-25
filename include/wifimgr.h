#ifndef ESPALTHERMA_WIFIMGR_H
#define ESPALTHERMA_WIFIMGR_H

// WiFi connection manager (ESP32 only), written for a weak link (~-89 dBm) to a
// UniFi AP that is re-provisioned every night (radio restart for 6-20 s).
//
// Why it exists (2026-11-25):
//  * Arduino-ESP32 2.0.17 leaves WiFi.status() at WL_CONNECTED after a disconnect
//    with reason AUTH_EXPIRE (2), so the old "if (WiFi.status() != WL_CONNECTED)"
//    recovery never ran. The link state is tracked from events here instead.
//  * The core's auto-reconnect and our own retry loop fought each other, and every
//    attempt was cut off after 15 s (a slow association + DHCP at -89 dBm needs longer).
//  * After 2 min the board rebooted and started from scratch again.
//
// Strategy: one owner of reconnection (core auto-reconnect off). Each attempt scans,
// picks a BSSID of our SSID (mostly the strongest, sometimes the others) and gets up to
// 30 s. Back-off 5 s -> 60 s. After 3 failures alternate attempts are made without PMF
// (in case the AP still holds a protected session for us), every 6 failures the WiFi
// stack is restarted, and only after 20 min offline the board reboots.
// Outage statistics live in RTC memory, survive those reboots and are published to
// espaltherma/diag (retained) once MQTT is back.

#ifndef ARDUINO_ARCH_ESP8266
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <PubSubClient.h>
#include "restart.h"
#include "statusled.h"

#define WIFI_ATTEMPT_TIMEOUT_MS 30000UL           // one association + DHCP attempt
#define WIFI_BACKOFF_MIN_MS 5000UL
#define WIFI_BACKOFF_MAX_MS 60000UL
#define WIFI_PMF_OFF_AFTER 3                      // failures before alternating PMF off
#define WIFI_STACK_RESET_EVERY 6                  // failures between WiFi stack restarts
#define WIFI_REBOOT_AFTER_MS (20UL * 60UL * 1000UL) // offline this long (per boot) -> reboot
#define WIFI_BEACON_TIMEOUT_S 12                  // default 6; ride out short AP hiccups
#define LOOP_WDT_TIMEOUT_S 300                    // hang watchdog for the loop task
#define WIFI_MAX_CANDIDATES 6
#define WIFI_REASON_RING 12

void handleScreen(); // main.cpp
void wakeScreen();   // main.cpp

//---------------------------------------------------------------- RTC diagnostics
#define WDIAG_MAGIC 0x57444932UL
struct WifiDiag
{
  uint32_t magic;
  uint32_t boots;           // software resets since power-on
  uint32_t outageMs;        // offline time of the current outage, over reboots
  uint32_t outageReboots;   // reboots done because of the current outage
  uint32_t outageAttempts;  // connection attempts in the current outage
  uint32_t outages;         // outages since power-on
  uint32_t disconnects;     // disconnect events since power-on
  uint8_t reasons[WIFI_REASON_RING]; // last disconnect reasons of the current outage
  uint8_t reasonIdx;
  uint8_t reasonCount;
};
RTC_NOINIT_ATTR WifiDiag wdiag;

static void wdiagAddReason(uint8_t r)
{
  wdiag.reasons[wdiag.reasonIdx] = r;
  wdiag.reasonIdx = (wdiag.reasonIdx + 1) % WIFI_REASON_RING;
  if (wdiag.reasonCount < WIFI_REASON_RING)
    wdiag.reasonCount++;
}

static void wdiagClearOutage()
{
  wdiag.outageMs = 0;
  wdiag.outageReboots = 0;
  wdiag.outageAttempts = 0;
  wdiag.reasonIdx = 0;
  wdiag.reasonCount = 0;
}

//---------------------------------------------------------------- state
static volatile bool wLinkUp = false;     // got an IP and no disconnect since
static volatile bool wAssociated = false; // L2 associated (maybe still waiting for DHCP)
static volatile bool wFailed = false;     // a disconnect event happened during the attempt
static volatile uint8_t wLastReason = 0;

struct WifiCand
{
  uint8_t bssid[6];
  int32_t channel;
  int32_t rssi;
};
static WifiCand wCands[WIFI_MAX_CANDIDATES];
static int wNumCands = 0;

static bool wAttemptActive = false;
static unsigned long wAttemptStart = 0;
static unsigned long wNextAttemptAt = 0;
static unsigned long wOfflineSince = 0; // 0 = online
static uint32_t wFailStreak = 0;
static uint32_t wAttemptNo = 0;
static bool wAttemptPmf = true;
static bool wConnectedPmf = true;
static bool wDiagPending = true; // publish once MQTT is up (also after boot)
static uint32_t wLastOutageS = 0;
static uint32_t wLastOutageReboots = 0;
static uint32_t wLastOutageAttempts = 0;
static uint8_t wLastOutageReasons[WIFI_REASON_RING];
static uint8_t wLastOutageReasonCount = 0;

static inline void feedWatchdog() { esp_task_wdt_reset(); }

bool wifiLinkUp()
{
  return wLinkUp && WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0;
}

static void wOnEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    wAssociated = true;
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    wLinkUp = true;
    Serial.printf("\nWiFi got IP %s, BSSID %s, ch %d, RSSI %d dBm\n", WiFi.localIP().toString().c_str(),
                  WiFi.BSSIDstr().c_str(), WiFi.channel(), WiFi.RSSI());
    break;
  case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    wLinkUp = false;
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    wLinkUp = false;
    wAssociated = false;
    wLastReason = info.wifi_sta_disconnected.reason;
    if (wLastReason == WIFI_REASON_ASSOC_LEAVE)
      break; // our own WiFi.disconnect(), not a failure
    wFailed = true;
    wdiag.disconnects++;
    wdiagAddReason(wLastReason);
    Serial.printf("\nWiFi disconnected, reason %d\n", wLastReason);
    break;
  default:
    break;
  }
}

static void wApplyRadioSettings()
{
  WiFi.persistent(false);       // no NVS write on every retry
  WiFi.setAutoReconnect(false); // we are the only one reconnecting
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // modem sleep loses beacons on a weak link
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_err_t e = esp_wifi_set_inactive_time(WIFI_IF_STA, WIFI_BEACON_TIMEOUT_S);
  if (e != ESP_OK)
    Serial.printf("esp_wifi_set_inactive_time failed: %d\n", e);
}

static void wScan()
{
  feedWatchdog();
  wNumCands = 0;
  int16_t n = WiFi.scanNetworks(false, true);
  for (int16_t i = 0; i < n; i++)
  {
    if (WiFi.SSID(i) != WIFI_SSID)
      continue;
    WifiCand c;
    memcpy(c.bssid, WiFi.BSSID(i), 6);
    c.channel = WiFi.channel(i);
    c.rssi = WiFi.RSSI(i);
    // insert sorted by RSSI, strongest first
    int pos = wNumCands < WIFI_MAX_CANDIDATES ? wNumCands : WIFI_MAX_CANDIDATES - 1;
    if (wNumCands == WIFI_MAX_CANDIDATES && c.rssi <= wCands[pos].rssi)
      continue;
    while (pos > 0 && wCands[pos - 1].rssi < c.rssi)
    {
      wCands[pos] = wCands[pos - 1];
      pos--;
    }
    wCands[pos] = c;
    if (wNumCands < WIFI_MAX_CANDIDATES)
      wNumCands++;
  }
  WiFi.scanDelete();
  Serial.printf("Scan: %d networks, %d '%s' APs:", n, wNumCands, WIFI_SSID);
  for (int i = 0; i < wNumCands; i++)
    Serial.printf(" %02x:%02x:%02x:%02x:%02x:%02x ch%d %ddBm;", wCands[i].bssid[0], wCands[i].bssid[1],
                  wCands[i].bssid[2], wCands[i].bssid[3], wCands[i].bssid[4], wCands[i].bssid[5],
                  wCands[i].channel, wCands[i].rssi);
  Serial.println();
}

static void wStartAttempt()
{
  feedWatchdog();
  WiFi.disconnect(); // drop any half-finished attempt
  delay(100);
  wScan();
  wFailed = false;
  wAssociated = false;
  wAttemptNo++;
  wdiag.outageAttempts++;

  // Every third attempt tries the next-best AP, the others the strongest one.
  int idx = 0;
  if (wNumCands > 1 && wAttemptNo % 3 == 0)
    idx = 1 + (wAttemptNo / 3) % (wNumCands - 1);
  // After a few failures, alternate between PMF capable and PMF off.
  wAttemptPmf = !(wFailStreak >= WIFI_PMF_OFF_AFTER && (wFailStreak % 2) == 1);

  if (wNumCands > 0)
    WiFi.begin(WIFI_SSID, WIFI_PWD, wCands[idx].channel, wCands[idx].bssid, false);
  else
    WiFi.begin(WIFI_SSID, WIFI_PWD, 0, NULL, false);

  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK)
  {
    conf.sta.pmf_cfg.capable = wAttemptPmf;
    conf.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }
  esp_wifi_connect();
  wAttemptActive = true;
  wAttemptStart = millis();
  Serial.printf("WiFi attempt %lu (streak %lu): %s, PMF %s\n", (unsigned long)wAttemptNo,
                (unsigned long)wFailStreak, wNumCands ? "AP from scan" : "no AP seen, blind connect",
                wAttemptPmf ? "on" : "off");
}

static void wStackReset()
{
  Serial.println("WiFi: restarting the WiFi stack");
  feedWatchdog();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
  wApplyRadioSettings();
}

static void wOnReconnected()
{
  unsigned long now = millis();
  uint32_t thisBootMs = wOfflineSince ? now - wOfflineSince : 0;
  wLastOutageS = (wdiag.outageMs + thisBootMs) / 1000;
  wLastOutageReboots = wdiag.outageReboots;
  wLastOutageAttempts = wdiag.outageAttempts;
  wLastOutageReasonCount = wdiag.reasonCount;
  for (int i = 0; i < wdiag.reasonCount; i++) // oldest first
    wLastOutageReasons[i] = wdiag.reasons[(wdiag.reasonIdx + WIFI_REASON_RING - wdiag.reasonCount + i) % WIFI_REASON_RING];
  wConnectedPmf = wAttemptPmf;
  if (wLastOutageS >= 10 || wLastOutageReboots > 0) // ignore the normal connect at boot
    wdiag.outages++;
  wdiagClearOutage();
  wOfflineSince = 0;
  wFailStreak = 0;
  wAttemptActive = false;
  wDiagPending = true;
  Serial.printf("WiFi back after %lu s (%lu attempts, %lu reboots)\n", (unsigned long)wLastOutageS,
                (unsigned long)wLastOutageAttempts, (unsigned long)wLastOutageReboots);
}

// Non-blocking: call often while offline.
void wifiService()
{
  feedWatchdog();
  if (wifiLinkUp())
  {
    if (wOfflineSince || wAttemptActive)
      wOnReconnected();
    return;
  }
  unsigned long now = millis();
  if (!wOfflineSince)
  {
    wOfflineSince = now ? now : 1;
    wNextAttemptAt = now + 2000; // let a restarting AP come back first
    Serial.printf("WiFi link down (last reason %d)\n", wLastReason);
  }

  if (wAttemptActive)
  {
    bool timedOut = now - wAttemptStart >= WIFI_ATTEMPT_TIMEOUT_MS;
    if ((wFailed && !wAssociated) || timedOut)
    {
      wAttemptActive = false;
      wFailStreak++;
      unsigned long backoff = WIFI_BACKOFF_MIN_MS << (wFailStreak > 4 ? 4 : wFailStreak - 1);
      if (backoff > WIFI_BACKOFF_MAX_MS)
        backoff = WIFI_BACKOFF_MAX_MS;
      Serial.printf("WiFi attempt failed (%s, reason %d), next in %lu s\n", timedOut ? "timeout" : "rejected",
                    wLastReason, backoff / 1000);
      if (wFailStreak % WIFI_STACK_RESET_EVERY == 0)
        wStackReset();
      wNextAttemptAt = millis() + backoff;
    }
  }
  else if ((long)(now - wNextAttemptAt) >= 0)
  {
    wStartAttempt();
  }

  if (wOfflineSince && millis() - wOfflineSince >= WIFI_REBOOT_AFTER_MS)
  {
    wdiag.outageMs += millis() - wOfflineSince;
    wdiag.outageReboots++;
    Serial.println("WiFi offline for 20 min, rebooting");
    restart_board();
  }
}

// Blocking: returns once we have an IP.
void checkWifi()
{
  if (wifiLinkUp())
  {
    wifiService(); // closes the books on a finished outage
    return;
  }
  ledSet(LED_WIFI);
  wakeScreen();
  unsigned long lastDot = 0;
  while (!wifiLinkUp())
  {
    wifiService();
    handleScreen();
    delay(50);
    if (millis() - lastDot >= 1000)
    {
      Serial.print(".");
      lastDot = millis();
    }
  }
  wifiService();
}

// Drop the link on purpose (e.g. MQTT keeps failing although WiFi looks up).
void wifiForceReconnect(const char *why)
{
  Serial.printf("WiFi: forcing reconnect (%s)\n", why);
  wLinkUp = false;
  WiFi.disconnect();
}

void wifiInit()
{
  esp_reset_reason_t rr = esp_reset_reason();
  if (wdiag.magic != WDIAG_MAGIC || rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT)
  {
    memset(&wdiag, 0, sizeof(wdiag));
    wdiag.magic = WDIAG_MAGIC;
  }
  else
  {
    wdiag.boots++;
  }
  // Hang watchdog for the loop task (reconfigures the IDF task WDT, 5 s by default).
  esp_task_wdt_init(LOOP_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  WiFi.onEvent(wOnEvent);
  wApplyRadioSettings();
}

static const char *resetReasonName(esp_reset_reason_t r)
{
  switch (r)
  {
  case ESP_RST_POWERON: return "poweron";
  case ESP_RST_SW: return "software";
  case ESP_RST_PANIC: return "panic";
  case ESP_RST_INT_WDT: return "int_wdt";
  case ESP_RST_TASK_WDT: return "task_wdt";
  case ESP_RST_WDT: return "wdt";
  case ESP_RST_BROWNOUT: return "brownout";
  case ESP_RST_DEEPSLEEP: return "deepsleep";
  case ESP_RST_EXT: return "external";
  default: return "other";
  }
}

// Called after every successful MQTT connect; publishes once per reconnect.
void wifiDiagPublish(PubSubClient &mqtt)
{
  if (!wDiagPending)
    return;
  wDiagPending = false;
  char reasons[64] = "";
  for (int i = 0; i < wLastOutageReasonCount; i++)
    snprintf(reasons + strlen(reasons), sizeof(reasons) - strlen(reasons), "%s%d", i ? "," : "", wLastOutageReasons[i]);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"uptime_s\":%lu,\"reset_reason\":\"%s\",\"boots\":%lu,\"outages\":%lu,\"disconnects\":%lu,"
           "\"last_outage_s\":%lu,\"last_outage_attempts\":%lu,\"last_outage_reboots\":%lu,\"last_outage_reasons\":[%s],"
           "\"bssid\":\"%s\",\"channel\":%d,\"rssi\":%d,\"pmf\":%s}",
           millis() / 1000, resetReasonName(esp_reset_reason()), (unsigned long)wdiag.boots, (unsigned long)wdiag.outages,
           (unsigned long)wdiag.disconnects, (unsigned long)wLastOutageS, (unsigned long)wLastOutageAttempts,
           (unsigned long)wLastOutageReboots, reasons, WiFi.BSSIDstr().c_str(), WiFi.channel(), WiFi.RSSI(),
           wConnectedPmf ? "true" : "false");
  mqtt.publish("espaltherma/diag", buf, true);
  mqtt.publish("espaltherma/log", buf);
}

#endif // !ARDUINO_ARCH_ESP8266
#endif
