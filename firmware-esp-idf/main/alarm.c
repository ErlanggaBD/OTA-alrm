#include "alarm.h"

#include <stdio.h>

#include "app_config.h"
#include "app_state.h"
#include "driver/gpio.h"
#include "log_buffer.h"

static uint32_t s_last_eval = 0;

static void write_buzzer(bool on) {
#if BUZZER_ACTIVE_HIGH
  gpio_set_level(BUZZER_PIN, on ? 1 : 0);
#else
  gpio_set_level(BUZZER_PIN, on ? 0 : 1);
#endif
}

static void write_relay(bool on) {
#if RELAY_ACTIVE_LOW
  gpio_set_level(RELAY_PIN, on ? 0 : 1);
#else
  gpio_set_level(RELAY_PIN, on ? 1 : 0);
#endif
}

static void write_led(bool on) {
#if LED_ACTIVE_HIGH
  gpio_set_level(STATUS_LED_PIN, on ? 1 : 0);
#else
  gpio_set_level(STATUS_LED_PIN, on ? 0 : 1);
#endif
}

static uint8_t evaluate_sensor(uint8_t idx) {
  const SensorState *s = &g_sensors[idx];
  const Threshold *t = &g_config.thresholds[idx];

  if (!s->valid) {
    if (s->error_count >= SENSOR_FAULT_AFTER_ERRORS) return ALARM_FAULT;
    return s->state;
  }

  float v = s->primary;
  float h = t->hysteresis;
  if (h < 0) h = 0;

  bool high_alarm = (v >= t->alarm_high) || (s->state == ALARM_ALARM && v > t->alarm_high - h);
  bool low_alarm = (v <= t->alarm_low) || (s->state == ALARM_ALARM && v < t->alarm_low + h);
  if (high_alarm || low_alarm) return ALARM_ALARM;

  bool high_warn = (v >= t->warn_high) || (s->state >= ALARM_WARNING && v > t->warn_high - h);
  bool low_warn = (v <= t->warn_low) || (s->state >= ALARM_WARNING && v < t->warn_low + h);
  if (high_warn || low_warn) return ALARM_WARNING;

  return ALARM_NORMAL;
}

static void update_summary(uint8_t global_state, uint32_t mask) {
  g_alarm.summary[0] = 0;
  if (global_state == ALARM_NORMAL) {
    snprintf(g_alarm.summary, sizeof(g_alarm.summary), "Semua sensor aktif dalam batas");
    return;
  }

  size_t used = 0;
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (!(mask & (1UL << i))) continue;
    int written = snprintf(g_alarm.summary + used, sizeof(g_alarm.summary) - used, "%s%s",
                           used ? ", " : "", g_config.labels[i]);
    if (written < 0) break;
    used += (size_t)written;
    if (used >= sizeof(g_alarm.summary)) break;
  }
}

void alarm_init(void) {
  gpio_config_t out = {
    .pin_bit_mask = (1ULL << BUZZER_PIN) | (1ULL << RELAY_PIN) | (1ULL << STATUS_LED_PIN),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&out);

  write_buzzer(false);
  write_relay(false);
  write_led(true);

  g_alarm.state = ALARM_NORMAL;
  g_alarm.latched = false;
  g_alarm.acknowledged = false;
  g_alarm.ack_time = 0;
  g_alarm.since = millis_now();
  g_alarm.active_mask = 0;
  snprintf(g_alarm.summary, sizeof(g_alarm.summary), "Sistem normal");
}

void alarm_loop(void) {
  if (millis_now() - s_last_eval < ALARM_EVAL_INTERVAL_MS) return;
  s_last_eval = millis_now();

  uint8_t global_state = ALARM_NORMAL;
  uint32_t mask = 0;

  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (i >= g_config.active_sensors) {
      g_sensors[i].state = ALARM_NORMAL;
      g_sensors[i].candidate_state = ALARM_NORMAL;
      g_sensors[i].candidate_count = 0;
      continue;
    }

    uint8_t candidate = evaluate_sensor(i);
    if (candidate != g_sensors[i].state) {
      if (candidate == g_sensors[i].candidate_state) {
        g_sensors[i].candidate_count++;
      } else {
        g_sensors[i].candidate_state = candidate;
        g_sensors[i].candidate_count = 1;
      }
      if (g_sensors[i].candidate_count >= ALARM_DEBOUNCE_SAMPLES) {
        g_sensors[i].state = candidate;
        g_sensors[i].candidate_count = 0;
      }
    } else {
      g_sensors[i].candidate_state = candidate;
      g_sensors[i].candidate_count = 0;
    }

    if (g_sensors[i].state > global_state) global_state = g_sensors[i].state;
    if (g_sensors[i].state != ALARM_NORMAL) mask |= (1UL << i);
  }

  uint8_t previous = g_alarm.state;
  g_alarm.state = global_state;
  g_alarm.active_mask = mask;
  if (global_state != previous) {
    g_alarm.since = millis_now();
    log_add(global_state >= ALARM_ALARM ? "error"
            : (global_state == ALARM_WARNING ? "warn" : "info"),
            "status alarm: %s", alarm_level_name(global_state));
  }

  if (global_state >= ALARM_ALARM) {
    g_alarm.latched = true;
    if (g_alarm.acknowledged && g_alarm.ack_time != 0 &&
        millis_now() - g_alarm.ack_time > RE_ALERT_MS) {
      g_alarm.acknowledged = false;
      g_alarm.ack_time = 0;
      log_add("warn", "alarm re-alert setelah acknowledge");
    }
  } else if (global_state == ALARM_NORMAL) {
    if (g_alarm.latched || g_alarm.acknowledged) {
      log_add("info", "kondisi normal, latch alarm dibersihkan");
    }
    g_alarm.latched = false;
    g_alarm.acknowledged = false;
    g_alarm.ack_time = 0;
  }

  update_summary(global_state, mask);

  bool buzzer_on = false;
  if (global_state == ALARM_WARNING) {
    buzzer_on = (millis_now() % 1000UL) < 250UL;
  } else if (global_state >= ALARM_ALARM) {
    buzzer_on = !g_alarm.acknowledged;
  }
  write_buzzer(buzzer_on);
  write_relay(global_state >= ALARM_ALARM);

  if (global_state == ALARM_NORMAL) {
    write_led(true);
  } else if (global_state == ALARM_WARNING) {
    write_led((millis_now() % 1000UL) < 500UL);
  } else {
    write_led((millis_now() % 400UL) < 200UL);
  }
}

void alarm_acknowledge(void) {
  if (g_alarm.state >= ALARM_ALARM) {
    g_alarm.acknowledged = true;
    g_alarm.ack_time = millis_now();
    log_add("info", "alarm di-acknowledge oleh admin");
  }
}

uint8_t alarm_level(void) {
  return g_alarm.state;
}

uint32_t alarm_active_mask(void) {
  return g_alarm.active_mask;
}
