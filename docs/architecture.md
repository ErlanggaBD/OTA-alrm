# Architecture

## Goals

1. The alarm function is local and must keep working with no Wi-Fi, no internet,
   and no backend.
2. Identity (Email ID + device secret) is written to non-volatile storage only
   after the vendor backend confirms provisioning.
3. The active sensor count is a vendor-controlled quota, but the web UI and alarm
   engine must adapt at runtime without reflashing.
4. The same dashboard works on both firmware implementations.

## High-level components

```
┌─────────────────────────────────────────────────────────────────┐
│ ESP32                                                            │
│                                                                  │
│  sensors.c/.cpp ──► alarm.c/.cpp ──► buzzer / relay / LED        │
│        │                 ▲                                       │
│        │                 │ thresholds, active count              │
│        ▼                 │                                       │
│  app_state (g_config, g_sensors, g_alarm, g_cloud)               │
│        ▲                 ▲                                       │
│        │                 │                                       │
│  storage (NVS)     web_server (HTTP :80) ◄── browser             │
│        ▲                 ▲                                       │
│        │                 │                                       │
│  provisioning      cloud_sync (HMAC, config, heartbeat)          │
│  (SoftAP+DNS)            │                                       │
│                          ▼                                       │
│                     ota_manager (web + cloud OTA)                │
└──────────────────────────┬───────────────────────────────────────┘
                           │ HTTPS / HTTP
                           ▼
                 ┌───────────────────────┐
                 │ Node.js backend        │
                 │ SQLite + admin UI      │
                 │ HMAC device auth       │
                 │ quota + firmware       │
                 └───────────────────────┘
```

## Module map

### Shared concepts (both firmwares)

| Concern | Arduino | ESP-IDF |
| --- | --- | --- |
| Build-time constants | `app_config.h` | `app_config.h` |
| Global state | `app_state.h/.cpp` | `app_state.h/.c` |
| NVS persistence | `storage.*` (Preferences) | `storage.*` (NVS blob) |
| Log ring buffer | `log_buffer.*` | `log_buffer.*` |
| Sensor driver | `sensors.*` (DHT) | `sensors.*` (DHT) |
| Alarm engine | `alarm.*` | `alarm.*` |
| Wi-Fi / NTP / health | `net_manager.*` | `net_manager.*` |
| Captive portal | `provisioning.*` | `provisioning.*` |
| Cloud sync + quota | `cloud_sync.*` | `cloud_sync.*` |
| OTA | `ota_manager.*` | `ota_manager.*` |
| HTTP + UI | `web_server.*` + `web_ui.h` | `web_server.*` + SPIFFS |
| Entry point | `MedGasAlarm.ino` | `main.c` (`app_main`) |

### Backend

| File | Purpose |
| --- | --- |
| `src/app.js` | Express app, static firmware, routes |
| `src/config.js` | Environment/config loading |
| `src/crypto.js` | SHA-256, HMAC, AES-256-GCM secret storage |
| `src/db.js` | SQLite schema, device/admin/event/firmware queries |
| `src/auth.js` | Device HMAC verification, signed responses |
| `src/routes/device.js` | Provision, config, heartbeat, firmware manifest |
| `src/routes/admin.js` | Admin login, devices, quota, firmware, events |
| `src/admin/views/` | EJS admin UI |

## Global state

`DeviceConfig` is the single source of truth for runtime configuration. It is
stored as one NVS blob:

- identity: `email`, `device_secret`, `serial`
- Wi-Fi: `ssid`, `wifi_pass`
- local auth: `admin_pass` (SHA-256 hex)
- vendor: `api_base`, `plan`, `quota_expires`, `last_sync`
- quota: `active_sensors`, `default_active_sensors`
- policy: `lock_policy`, `offline_action`, `grace_days`, `allow_local_override`
- sensor metadata: `labels[8]`, `thresholds[8]`
- OTA: `fw_version`, `fw_available_version`, `fw_url`, `fw_sha`, `fw_size`

`SensorState` holds the last reading, validity, error count, and debounced alarm
state for each channel. `AlarmState` holds the global level, latch/ack state,
active mask, and a human-readable summary. `CloudState` holds online/sync status
and the last error.

`g_provisioned`, `g_ap_mode`, `g_ui_locked`, and the OTA status fields are
separate globals because they are read by the HTTP handlers on every request.

## Boot state machine

```
power on
   │
   ├─ load NVS
   │
   ├─ not provisioned ──────────────► SoftAP + captive portal (setup.html)
   │                                     │
   │                                     ├─ provision OK ─► save NVS ─► reboot
   │                                     └─ fail ─────────► stay in portal
   │
   └─ provisioned
         │
         ├─ Wi-Fi connects ──────────► dashboard + cloud sync + OTA
         │
         └─ Wi-Fi fails ─────────────► recovery SoftAP + portal
                                          │
                                          └─ fix Wi-Fi/server ─► reboot
```

The alarm engine starts before the network state is known, so the buzzer/relay
are valid even while the device is in the captive portal.

## Alarm engine

- Reads every active channel every `SENSOR_READ_INTERVAL_MS` (2 s by default).
- A channel becomes `fault` after `SENSOR_FAULT_AFTER_ERRORS` consecutive read
  failures.
- State changes are debounced by `ALARM_DEBOUNCE_SAMPLES` evaluations.
- Thresholds have separate warning/alarm high/low values plus hysteresis.
- Global level is the maximum of all active channel states.
- `alarm` latches; the dashboard can acknowledge it. If the condition persists,
  the buzzer re-alerts after `RE_ALERT_MS`.
- Buzzer patterns: warning = short beep, alarm = continuous until acknowledged,
  normal = off. Relay closes on alarm. LED indicates normal/warning/alarm.
- **The engine never consults the internet.** Lock policy only affects the UI.

## Quota and lock policy

`cloud_apply_config()` maps the backend config payload into `DeviceConfig`:

- `active_sensors` is clamped to `1..8`.
- `sensor_labels` and `thresholds` are copied into the fixed-size arrays.
- `lock_policy` is one of `provision_only`, `grace`, `strict`.
- `offline_action` is one of `keep_last`, `fallback_default`, `lock_ui`.
- `quota.expires_epoch` is stored as epoch seconds.
- `firmware.*` is stored for cloud OTA.

`cloud_apply_policy()` then decides whether the dashboard is locked:

| Policy | Behaviour |
| --- | --- |
| `provision_only` | Dashboard is available once provisioned; quota expiry does not lock it. |
| `grace` | If the last successful sync is older than `grace_days`, apply `offline_action`. |
| `strict` | Dashboard locks when there is no internet/NTP. Alarm outputs keep running. |

When quota expires or sync is stale, `offline_action` decides:

- `keep_last` — keep the last active count, unlock the UI.
- `fallback_default` — set `active_sensors = default_active_sensors`, save, unlock.
- `lock_ui` — lock the dashboard; alarm engine still uses the last active count.

The local settings page may change `lock_policy` only when
`allow_local_override` is true. Otherwise the backend is authoritative.

## Concurrency

The web server, cloud sync, and OTA run in different contexts, so `g_config` is
protected by a recursive mutex:

- Arduino: `configLock()` / `configUnlock()` around `portMUX`/FreeRTOS recursive
  mutex semantics.
- ESP-IDF: `config_lock()` / `config_unlock()` around
  `xSemaphoreCreateRecursiveMutex()`.

Rules:

- Copy strings out of `g_config` while holding the lock, then release it before
  blocking on network I/O.
- Never hold the config lock while calling `esp_http_client` or `HTTPClient`.
- `storage_save()` is called while the lock is held, because it serializes the
  same struct.

The ESP-IDF cloud sync and cloud OTA run in dedicated FreeRTOS tasks. The Arduino
implementation uses the same pattern via `xTaskCreate` for cloud sync/OTA so the
web server and alarm loop are not blocked by long downloads.

## Data flow: cloud sync

1. `cloud_loop()` checks interval and connectivity.
2. A background task copies `email`, `device_secret`, and `api_base` under lock.
3. It builds `GET\n/api/v1/devices/<email>/config\n<timestamp>\nSHA256("")`.
4. It signs the canonical string with HMAC-SHA256 and sends
   `X-Device-Email`, `X-Timestamp`, `X-Nonce`, `X-Signature`.
5. It verifies `X-Signature` on the exact response body.
6. It applies the JSON config, saves NVS, updates `last_sync`, then evaluates the
   lock policy.
7. A best-effort heartbeat is sent; a heartbeat failure does not invalidate the
   config.

## Data flow: OTA

- **Local upload**: browser sends the raw `.bin` to `POST /api/ota` with
  `X-Firmware-SHA256`. The firmware writes chunks to the inactive OTA partition,
  verifies SHA-256, calls `set_boot_partition`, and reboots.
- **Cloud OTA**: backend config includes `firmware.url`, `sha256`, `size`.
  The device downloads in a background task, verifies size and SHA-256, then sets
  the boot partition and reboots.
- **Arduino IDE OTA**: the Arduino core OTA service is enabled; select the
  device's network port in the IDE and upload normally. See `docs/ota.md`.

## Failure handling

| Failure | Behaviour |
| --- | --- |
| NVS config corrupt | Defaults are loaded; device appears unprovisioned. |
| Wi-Fi fails after provisioning | Recovery portal; identity is preserved. |
| Backend unreachable | Cloud marked offline; config/policy uses last known values. |
| NTP unavailable | Sync is deferred; `strict` may lock the UI. |
| Sensor read failures | Channel becomes `fault` after 3 errors. |
| OTA download/write fails | Status `error`, device stays on the current firmware. |
| OTA SHA mismatch | OTA aborted before boot partition switch. |
| Power loss during OTA | Bootloader keeps the previous valid app slot. |
