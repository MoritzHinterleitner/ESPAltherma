#ifndef ESPALTHERMA_STATUSLED_H
#define ESPALTHERMA_STATUSLED_H

// Debug status on the on-board NeoPixel (e.g. Adafruit QT Py ESP32-S3).
// Compiles to no-ops on boards without PIN_NEOPIXEL.
//
//   white  solid          booting / setting up (firmware is running, not the bootloader)
//   blue   blinking       connecting to WiFi
//   yellow blinking       WiFi OK, connecting to MQTT
//   cyan   solid          querying the heat pump over serial
//   green  short blip/2s  all OK: last heat pump read succeeded and MQTT is up
//   magenta short blip/2s MQTT OK, but the heat pump did not answer (check X10A cable)
//   white  fast blink     OTA update in progress
//   red    fast blink     about to reboot
//
// Note: the TinyUF2 bootloader uses the same LED: purple = 0.5 s boot check,
// solid red = stuck in the bootloader (not mounted), solid green = bootloader
// mounted as a USB drive. None of the firmware states are solid red/green/purple.

#include <Arduino.h>

enum LedState : uint8_t
{
  LED_BOOT,
  LED_WIFI,
  LED_MQTT,
  LED_QUERY,
  LED_OK,
  LED_NO_HEATPUMP,
  LED_OTA,
  LED_REBOOT
};

#if defined(PIN_NEOPIXEL) && !defined(ARDUINO_ARCH_ESP8266)

#ifndef LED_BRIGHTNESS
#define LED_BRIGHTNESS 40 // 0-255, the QT Py pixel is very bright
#endif

static volatile LedState ledState = LED_BOOT;
static unsigned long ledLastChange = 0;
static bool ledOn = false;
static bool ledInitDone = false;

static void ledRaw(uint8_t r, uint8_t g, uint8_t b)
{
  neopixelWrite(PIN_NEOPIXEL, (uint16_t)r * LED_BRIGHTNESS / 255,
                (uint16_t)g * LED_BRIGHTNESS / 255, (uint16_t)b * LED_BRIGHTNESS / 255);
}

static void ledColor(LedState s)
{
  switch (s)
  {
  case LED_BOOT:        ledRaw(255, 255, 255); break;
  case LED_WIFI:        ledRaw(0, 0, 255); break;
  case LED_MQTT:        ledRaw(255, 160, 0); break;
  case LED_QUERY:       ledRaw(0, 255, 255); break;
  case LED_OK:          ledRaw(0, 255, 0); break;
  case LED_NO_HEATPUMP: ledRaw(255, 0, 255); break;
  case LED_OTA:         ledRaw(255, 255, 255); break;
  case LED_REBOOT:      ledRaw(255, 0, 0); break;
  }
}


// Drives the blink patterns. Runs in its own FreeRTOS task (ledTask) so the
// LED keeps blinking even while the main loop is blocked (WiFi scan, MQTT
// connect timeouts, serial queries).
static LedState ledShown = LED_BOOT;
static void ledStep()
{
  if (ledState != ledShown)
  { //state changed: restart the pattern with the LED on
    ledShown = ledState;
    ledOn = true;
    ledLastChange = millis();
    ledColor(ledShown);
    return;
  }
  unsigned long now = millis();
  unsigned long onMs, offMs;
  switch (ledShown)
  {
  case LED_WIFI:
  case LED_MQTT:        onMs = 250; offMs = 250; break;
  case LED_OK:
  case LED_NO_HEATPUMP: onMs = 80; offMs = 1920; break;
  case LED_OTA:
  case LED_REBOOT:      onMs = 80; offMs = 80; break;
  default:              return; // solid states
  }
  if (now - ledLastChange >= (ledOn ? onMs : offMs))
  {
    ledOn = !ledOn;
    ledLastChange = now;
    if (ledOn)
      ledColor(ledShown);
    else
      ledRaw(0, 0, 0);
  }
}

static void ledTask(void *)
{
  for (;;)
  {
    ledStep();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void ledInit()
{
#ifdef NEOPIXEL_POWER
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
#endif
  ledInitDone = true;
  ledColor(LED_BOOT);
  ledOn = true;
  xTaskCreatePinnedToCore(ledTask, "statusled", 2048, nullptr, 1, nullptr, 0);
}

void ledTick() {} // kept for callers; the LED task does the work

void ledSet(LedState s)
{
  ledState = s; // picked up by the LED task within 20 ms
}

// Blocking red fast blink, used right before a reboot so it is visible.
void ledRebootFlash()
{
  if (!ledInitDone)
    return;
  ledSet(LED_REBOOT);
  unsigned long start = millis();
  while (millis() - start < 1500)
    delay(10);
}

#else
void ledInit() {}
void ledTick() {}
void ledSet(LedState) {}
void ledRebootFlash() {}
#endif

#endif
