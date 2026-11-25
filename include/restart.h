#ifndef ESPALTHERMA_RESTART_H
#define ESPALTHERMA_RESTART_H

#include <Arduino.h>
#include "statusled.h"

void restart_board()
{
  ledRebootFlash();
  #if defined(ARDUINO_ARCH_ESP8266)
  system_restart();
  #else
  esp_restart();
  #endif
}

#endif