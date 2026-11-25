#ifndef ESPALTHERMA_PROBE_H
#define ESPALTHERMA_PROBE_H

// Phase 2 reverse engineering: probe protocol-S registers that the definition file doesn't read.
//
// For every register in PROBE_REGISTERS (defined in the def file) the same READ request the normal
// queries use is sent ([0x02][reg][CRC]), and whatever comes back within 400 ms is captured, whatever
// its length. The raw reply goes into the ATTR JSON as one hex string per register, e.g.
//   "P51":"51 00 1a ... crc"   (a reply),  "P58":"15ea" (register not supported),  "P5A":"" (no answer).
// No labels are created, so this adds no Home Assistant entities: the recorder keeps ATTR as attributes
// of sensor.<host>_althermasensors, which is where the analysis reads it from.
//
// Only read requests are sent. One request per register per cycle, no retries.

#ifdef PROBE_REGISTERS
static const uint8_t probeRegs[] = {PROBE_REGISTERS};

static void probeRegister(uint8_t reg, char *out, size_t outLen)
{
  uint8_t query[3] = {0x02, reg, 0};
  query[2] = getCRC(query, 2);
  while (MySerial.available()) // drop anything left over from the previous query
    MySerial.read();
  MySerial.write(query, 3);

  uint8_t buf[48];
  size_t len = 0;
  unsigned long start = millis(), last = 0;
  while (millis() - start < 400 && len < sizeof(buf))
  {
    if (MySerial.available())
    {
      buf[len++] = MySerial.read();
      last = millis();
    }
    else if (len > 0 && millis() - last > 60) // ~60 byte times of silence: the reply is complete
      break;
    else
      delay(1);
  }
  out[0] = '\0';
  for (size_t i = 0; i < len && strlen(out) + 3 < outLen; i++)
    snprintf(out + strlen(out), outLen - strlen(out), "%02x", buf[i]);
}

// Appends "P<reg>":"<hex>" for every probe register to jsonbuff (call before sendValues()).
void appendProbes()
{
  char hex[2 * 48 + 1];
  for (size_t i = 0; i < sizeof(probeRegs); i++)
  {
    probeRegister(probeRegs[i], hex, sizeof(hex));
    snprintf(jsonbuff + strlen(jsonbuff), MAX_MSG_SIZE - strlen(jsonbuff), "\"P%02X\":\"%s\",", probeRegs[i], hex);
    delay(50);
  }
}
#else
void appendProbes() {}
#endif

#endif
