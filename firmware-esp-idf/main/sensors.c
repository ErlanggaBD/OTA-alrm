#include "sensors.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "log_buffer.h"

const int SENSOR_PINS[SENSOR_COUNT] = {21, 22, 23, 25, 26, 27, 32, 33};

static uint32_t s_last_read = 0;
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static bool wait_level(int pin, int level, uint32_t timeout_us) {
  uint32_t start = (uint32_t)esp_timer_get_time();
  while (gpio_get_level(pin) != level) {
    if ((uint32_t)esp_timer_get_time() - start > timeout_us) return false;
  }
  return true;
}

// DHT22 one-wire read. The whole 40-bit frame is read with interrupts
// disabled because the pulse widths (26 us vs 70 us) are timing critical.
// DHT22 is only the demo sensor; replace this driver for real gas probes.
static bool dht_read(int pin, float *temperature, float *humidity) {
  uint8_t data[5] = {0, 0, 0, 0, 0};

  gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
  gpio_set_level(pin, 0);
  esp_rom_delay_us(1200);
  gpio_set_level(pin, 1);
  esp_rom_delay_us(30);
  gpio_set_direction(pin, GPIO_MODE_INPUT);

  portENTER_CRITICAL(&s_dht_mux);

  bool ok = wait_level(pin, 0, 200) &&
            wait_level(pin, 1, 200) &&
            wait_level(pin, 0, 200);

  if (ok) {
    for (int i = 0; i < 40; i++) {
      if (!wait_level(pin, 1, 100)) {
        ok = false;
        break;
      }
      uint32_t start = (uint32_t)esp_timer_get_time();
      if (!wait_level(pin, 0, 150)) {
        ok = false;
        break;
      }
      uint32_t width = (uint32_t)esp_timer_get_time() - start;
      data[i / 8] <<= 1;
      if (width > 50) data[i / 8] |= 1;
    }
  }

  portEXIT_CRITICAL(&s_dht_mux);

  if (!ok) return false;

  uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
  if (checksum != data[4]) return false;

  uint16_t raw_hum = ((uint16_t)data[0] << 8) | data[1];
  uint16_t raw_temp = ((uint16_t)data[2] << 8) | data[3];
  int16_t temp = (int16_t)(raw_temp & 0x7FFF);
  if (raw_temp & 0x8000) temp = -temp;

  *temperature = temp / 10.0f;
  *humidity = raw_hum / 10.0f;
  return true;
}

void sensors_init(void) {
  for (int i = 0; i < SENSOR_COUNT; i++) {
    gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << SENSOR_PINS[i],
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

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
  log_add("info", "sensor DHT22 siap pada %u channel", (unsigned)SENSOR_COUNT);
}

static void read_one(uint8_t idx) {
  float t = NAN;
  float h = NAN;
  bool ok = dht_read(SENSOR_PINS[idx], &t, &h) && !isnan(t) && !isnan(h);

  if (ok) {
    bool was_fault = !g_sensors[idx].valid &&
                     g_sensors[idx].error_count >= SENSOR_FAULT_AFTER_ERRORS;
    g_sensors[idx].primary = t;
    g_sensors[idx].secondary = h;
    g_sensors[idx].valid = true;
    g_sensors[idx].error_count = 0;
    g_sensors[idx].last_read_ms = millis_now();
    g_sensors[idx].last_read_epoch = now_epoch();
    if (was_fault) log_add("info", "sensor %u pulih", (unsigned)(idx + 1));
  } else {
    if (g_sensors[idx].error_count < 255) g_sensors[idx].error_count++;
    if (g_sensors[idx].error_count >= SENSOR_FAULT_AFTER_ERRORS) {
      if (g_sensors[idx].valid) {
        log_add("error", "sensor %u fault (gagal baca %u x)", (unsigned)(idx + 1),
                (unsigned)g_sensors[idx].error_count);
      }
      g_sensors[idx].valid = false;
    }
  }
}

void sensors_loop(void) {
  if (millis_now() - s_last_read < SENSOR_READ_INTERVAL_MS) return;
  s_last_read = millis_now();

  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    read_one(i);
  }
}

uint32_t sensor_age_seconds(uint8_t idx) {
  if (idx >= SENSOR_COUNT || g_sensors[idx].last_read_ms == 0) return 0;
  return (millis_now() - g_sensors[idx].last_read_ms) / 1000UL;
}
