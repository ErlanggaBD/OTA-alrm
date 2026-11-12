#include "alarm.h"
#include "app_config.h"
#include "app_state.h"
#include "log_buffer.h"

static uint32_t s_lastEval = 0;

static void writeBuzzer(bool on) {
#if BUZZER_ACTIVE_HIGH
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BUZZER_PIN, on ? LOW : HIGH);
#endif
}

static void writeRelay(bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
#else
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
#endif
}

static void writeLed(bool on) {
#if LED_ACTIVE_HIGH
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#endif
}

static uint8_t evaluateSensor(uint8_t idx) {
  const SensorState& s = g_sensors[idx];
  const Threshold& t = g_config.thresholds[idx];

  if (!s.valid) {
    if (s.error_count >= SENSOR_FAULT_AFTER_ERRORS) return ALARM_FAULT;
    return s.state;
  }

  float v = s.primary;
  float h = t.hysteresis;
  if (h < 0) h = 0;

  bool highAlarm = (v >= t.alarm_high) || (s.state == ALARM_ALARM && v > t.alarm_high - h);
  bool lowAlarm = (v <= t.alarm_low) || (s.state == ALARM_ALARM && v < t.alarm_low + h);
  if (highAlarm || lowAlarm) return ALARM_ALARM;

  bool highWarn = (v >= t.warn_high) || (s.state >= ALARM_WARNING && v > t.warn_high - h);
  bool lowWarn = (v <= t.warn_low) || (s.state >= ALARM_WARNING && v < t.warn_low + h);
  if (highWarn || lowWarn) return ALARM_WARNING;

  return ALARM_NORMAL;
}

static void updateSummary(uint8_t globalState, uint32_t mask) {
  g_alarm.summary[0] = 0;
  if (globalState == ALARM_NORMAL) {
    snprintf(g_alarm.summary, sizeof(g_alarm.summary), "Semua sensor aktif dalam batas");
    return;
  }

  size_t used = 0;
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (!(mask & (1UL << i))) continue;
    int written = snprintf(
      g_alarm.summary + used,
      sizeof(g_alarm.summary) - used,
      "%s%s",
      used ? ", " : "",
      g_config.labels[i]
    );
    if (written < 0) break;
    used += (size_t)written;
    if (used >= sizeof(g_alarm.summary)) break;
  }
}

void alarmBegin() {
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  writeBuzzer(false);
  writeRelay(false);
  writeLed(true);

  g_alarm.state = ALARM_NORMAL;
  g_alarm.latched = false;
  g_alarm.acknowledged = false;
  g_alarm.ack_time = 0;
  g_alarm.since = millis();
  g_alarm.active_mask = 0;
  snprintf(g_alarm.summary, sizeof(g_alarm.summary), "Sistem normal");
}

void alarmLoop() {
  if (millis() - s_lastEval < ALARM_EVAL_INTERVAL_MS) return;
  s_lastEval = millis();

  uint8_t globalState = ALARM_NORMAL;
  uint32_t mask = 0;

  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    if (i >= g_config.active_sensors) {
      g_sensors[i].state = ALARM_NORMAL;
      g_sensors[i].candidate_state = ALARM_NORMAL;
      g_sensors[i].candidate_count = 0;
      continue;
    }

    uint8_t candidate = evaluateSensor(i);
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

    if (g_sensors[i].state > globalState) globalState = g_sensors[i].state;
    if (g_sensors[i].state != ALARM_NORMAL) mask |= (1UL << i);
  }

  uint8_t previous = g_alarm.state;
  g_alarm.state = globalState;
  g_alarm.active_mask = mask;
  if (globalState != previous) {
    g_alarm.since = millis();
    logAdd(
      globalState >= ALARM_ALARM ? "error" : (globalState == ALARM_WARNING ? "warn" : "info"),
      "status alarm: %s",
      alarmLevelName(globalState)
    );
  }

  if (globalState >= ALARM_ALARM) {
    g_alarm.latched = true;
    if (g_alarm.acknowledged && g_alarm.ack_time != 0 &&
        millis() - g_alarm.ack_time > RE_ALERT_MS) {
      g_alarm.acknowledged = false;
      g_alarm.ack_time = 0;
      logAdd("warn", "alarm re-alert setelah acknowledge");
    }
  } else if (globalState == ALARM_NORMAL) {
    if (g_alarm.latched || g_alarm.acknowledged) {
      logAdd("info", "kondisi normal, latch alarm dibersihkan");
    }
    g_alarm.latched = false;
    g_alarm.acknowledged = false;
    g_alarm.ack_time = 0;
  }

  updateSummary(globalState, mask);

  bool buzzerOn = false;
  if (globalState == ALARM_WARNING) {
    buzzerOn = (millis() % 1000UL) < 250UL;
  } else if (globalState >= ALARM_ALARM) {
    buzzerOn = !g_alarm.acknowledged;
  }
  writeBuzzer(buzzerOn);
  writeRelay(globalState >= ALARM_ALARM);

  if (globalState == ALARM_NORMAL) {
    writeLed(true);
  } else if (globalState == ALARM_WARNING) {
    writeLed((millis() % 1000UL) < 500UL);
  } else {
    writeLed((millis() % 400UL) < 200UL);
  }
}

void alarmAcknowledge() {
  if (g_alarm.state >= ALARM_ALARM) {
    g_alarm.acknowledged = true;
    g_alarm.ack_time = millis();
    logAdd("info", "alarm di-acknowledge oleh admin");
  }
}

uint8_t alarmLevel() {
  return g_alarm.state;
}

uint32_t alarmActiveMask() {
  return g_alarm.active_mask;
}
