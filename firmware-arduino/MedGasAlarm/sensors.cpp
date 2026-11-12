#include "sensors.h"
#include "app_config.h"
#include "app_state.h"
#include "log_buffer.h"

#include <DHT.h>

static DHT* s_dht[SENSOR_COUNT] = {nullptr};
static uint32_t s_lastRead = 0;

void sensorsBegin() {
  for (int i = 0; i < SENSOR_COUNT; i++) {
    s_dht[i] = new DHT(SENSOR_PINS[i], DHT22);
    s_dht[i]->begin();

    g_sensors[i].primary = NAN;
    g_sensors[i].secondary = NAN;
    g_sensors[i].valid = false;
    g_sensors[i].error_count = 0;
    g_sensors[i].last_read_ms = 0;
    g_sensors[i].last_read_epoch = 0;
    g_sensors[i].state = ALARM_NORMAL;
    g_sensors[i].candidate_state = ALARM_NORMAL;
    g_sensors[i].candidate_count = 0;
  }
  logAdd("info", "sensor DHT22 siap pada %u channel", (unsigned)SENSOR_COUNT);
}

static void readOne(uint8_t idx) {
  if (!s_dht[idx]) return;

  float t = s_dht[idx]->readTemperature();
  float h = s_dht[idx]->readHumidity();
  bool ok = !isnan(t) && !isnan(h);

  if (ok) {
    bool wasFault = g_sensors[idx].valid == false && g_sensors[idx].error_count >= SENSOR_FAULT_AFTER_ERRORS;
    g_sensors[idx].primary = t;
    g_sensors[idx].secondary = h;
    g_sensors[idx].valid = true;
    g_sensors[idx].error_count = 0;
    g_sensors[idx].last_read_ms = millis();
    g_sensors[idx].last_read_epoch = nowEpoch();
    if (wasFault) {
      logAdd("info", "sensor %u pulih", (unsigned)(idx + 1));
    }
  } else {
    if (g_sensors[idx].error_count < 255) g_sensors[idx].error_count++;
    if (g_sensors[idx].error_count >= SENSOR_FAULT_AFTER_ERRORS) {
      if (g_sensors[idx].valid) {
        logAdd("error", "sensor %u fault (gagal baca %u x)", (unsigned)(idx + 1), (unsigned)g_sensors[idx].error_count);
      }
      g_sensors[idx].valid = false;
    }
  }
}

void sensorsLoop() {
  if (millis() - s_lastRead < SENSOR_READ_INTERVAL_MS) return;
  s_lastRead = millis();

  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    readOne(i);
  }
}

uint32_t sensorAgeSeconds(uint8_t idx) {
  if (idx >= SENSOR_COUNT || g_sensors[idx].last_read_ms == 0) return 0;
  return (millis() - g_sensors[idx].last_read_ms) / 1000UL;
}
