# API Reference

Two APIs are involved:

1. **Local device API** — served by the ESP32 on port 80 for the dashboard and
   captive portal.
2. **Vendor backend API** — served by the Node.js reference server under
   `/api/v1`, consumed by the device with HMAC authentication.

---

# Local device API

Base URL: `http://<device-ip>/` (or `http://192.168.4.1/` in AP mode).

## Authentication

- Login creates an HTTP-only cookie `mgsid` (30-minute sliding TTL, max 4
  sessions).
- `GET /api/system` is public and returns a `csrf` token. All state-changing
  requests must send it as:

```http
X-CSRF-Token: <csrf>
```

- Five failed logins lock login for 60 seconds.
- Endpoints marked **locked** return HTTP 423 when the dashboard lock policy is
  active. The alarm engine keeps running.

## Pages

| Path | Purpose |
| --- | --- |
| `/` | Dashboard when provisioned, otherwise setup page |
| `/setup` | Captive portal form |
| `/login` | Admin login |
| `/settings` | Thresholds, policy, OTA, maintenance |
| `/locked` | Locked-dashboard explanation page |
| `/style.css`, `/app.js` | Shared UI assets |

## `GET /api/system`

Public. Returns device, cloud, alarm, sensor-count, and OTA status.

```json
{
  "provisioned": true,
  "ap_mode": false,
  "locked": false,
  "lock_reason": "",
  "csrf": "…",
  "ap_ssid": "MedGasAlarm-1A2B",
  "ap_pass": "medgas-1A2B",
  "device": {
    "serial": "MG-1A2B3C",
    "hw": "esp32-devkit-4mb",
    "fw": "1.0.0",
    "uptime": 3600,
    "ip": "192.168.1.50",
    "rssi": -58,
    "ssid": "Hospital-WiFi",
    "email": "ward-icu@hospital.example"
  },
  "cloud": {
    "online": true,
    "last_sync": 1760000000,
    "last_error": "",
    "plan": "basic",
    "quota_expires": 1791536000,
    "policy": "provision_only",
    "grace_days": 7,
    "offline_action": "keep_last"
  },
  "alarm": {
    "state": "normal",
    "latched": false,
    "acknowledged": false,
    "summary": "Semua sensor aktif dalam batas",
    "triggered": []
  },
  "sensors": { "active": 4, "total": 8 },
  "ota": {
    "available": false,
    "version": "",
    "size": 0,
    "status": "idle",
    "progress": 0,
    "error": ""
  }
}
```

## `GET /api/sensors`

Requires login and an unlocked dashboard. Returns **only the active channels**,
so inactive sensor cards never reach the browser.

```json
{
  "sensors": [
    {
      "idx": 0,
      "label": "Sensor 1",
      "unit": "C",
      "secondary_unit": "%",
      "valid": true,
      "fault": false,
      "state": "normal",
      "age": 1,
      "last_read": 1760000000,
      "value": 27.4,
      "secondary": 61.2
    }
  ]
}
```

`value`/`secondary` are `null` when the reading is invalid. `state` is one of
`normal`, `warning`, `alarm`, `fault`.

## `GET /api/config`

Requires login and an unlocked dashboard.

```json
{
  "active_sensors": 4,
  "default_active_sensors": 4,
  "lock_policy": "provision_only",
  "allow_local_override": true,
  "auto_ota": false,
  "grace_days": 7,
  "offline_action": "keep_last",
  "sensor_labels": ["Sensor 1", "Sensor 2", "…"],
  "thresholds": [
    {
      "idx": 0,
      "warn_low": 5,
      "alarm_low": 2,
      "warn_high": 35,
      "alarm_high": 40,
      "hysteresis": 0.5,
      "unit": "C"
    }
  ],
  "quota": { "plan": "basic", "expires_epoch": 1791536000, "expires_at": "2026-10-09T00:00:00Z" },
  "firmware": { "version": "", "url": "", "sha256": "", "size": 0 },
  "server_time": 1760000000,
  "issued_at": 1760000000,
  "expires_at": 1760086400
}
```

## `POST /api/config`

Requires login, CSRF, and an unlocked dashboard. All fields are optional; at
least one must be present.

```json
{
  "lock_policy": "grace",
  "sensor_labels": ["ICU O2", "ICU CO2", "…"],
  "thresholds": [
    { "idx": 0, "warn_low": 5, "alarm_low": 2, "warn_high": 35, "alarm_high": 40, "hysteresis": 0.5, "unit": "C" }
  ]
}
```

Rules:

- `lock_policy`: `provision_only`, `grace`, `strict`.
- Thresholds must satisfy `alarm_low <= warn_low < warn_high <= alarm_high`.
- `allow_local_override` must be true to change `lock_policy` locally.

Response: `{ "ok": true, "message": "Konfigurasi disimpan" }`.

## `POST /api/login`

```json
{ "password": "admin12345" }
```

Success sets `mgsid` and returns `{ "ok": true, "message": "Login berhasil" }`.
Wrong password returns HTTP 401; too many attempts returns HTTP 429.

## `POST /api/logout`

Destroys the session and clears the cookie.

## `POST /api/alarm/ack`

Acknowledges an active alarm. Requires login, CSRF, and an unlocked dashboard.

## `POST /api/cloud/sync`

Requests an immediate cloud sync. Requires login and CSRF.

- `503` if Wi-Fi is down.
- `409` if a sync is already running.
- `200 { "ok": true, "message": "Sync cloud dimulai" }` otherwise.

## `GET /api/logs?limit=40`

Requires login. Returns newest-first log entries (max 80).

```json
{ "logs": [ { "time": "12:34:56", "level": "info", "message": "sync cloud sukses" } ] }
```

## `POST /api/ota`

Raw firmware upload. Requires login, CSRF, and an unlocked dashboard.

```http
Content-Type: application/octet-stream
X-Firmware-SHA256: <optional 64 hex chars>
```

Body: the application `.bin`. On success:

```json
{ "ok": true, "message": "Firmware terpasang. Alat akan reboot.", "progress": 100 }
```

## `GET /api/ota/status`

Requires login.

```json
{ "status": "downloading", "progress": 42, "error": "", "reboot_at": 0 }
```

`reboot_at` is a device uptime deadline in milliseconds (`0` = none).

## `POST /api/ota/cloud`

Starts a cloud OTA download. Requires login, CSRF, and an unlocked dashboard.
Returns `400` if no firmware is available for this device, `409` if another OTA
is active.

## `POST /api/factory-reset`

Requires login and CSRF.

```json
{ "confirm": "RESET" }
```

Erases NVS and reboots into the first-boot portal.

## `POST /api/reboot`

Requires login and CSRF. Reboots after a short delay.

## `GET /api/scan`

Only available in AP mode. Returns nearby networks.

```json
{ "networks": [ { "ssid": "Hospital-WiFi", "rssi": -61, "secure": true } ] }
```

## `POST /api/provision`

Only available in AP mode. Requires the CSRF token from `/api/system`.

```json
{
  "email": "ward-icu@hospital.example",
  "ssid": "Hospital-WiFi",
  "password": "wifi-password",
  "admin_password": "new-admin-password",
  "api_base": "https://api.vendor.example"
}
```

Success:

```json
{ "ok": true, "message": "Registrasi berhasil. Alat akan reboot ke dashboard." }
```

Failure returns `400` with a human-readable `error`.

---

# Vendor backend API

Base URL: `PUBLIC_BASE_URL` (default `http://localhost:3000`).

## `GET /api/v1/health`

Public.

```json
{ "ok": true, "service": "medgas-alarm-backend", "server_time": 1760000000 }
```

## `POST /api/v1/provision`

Public (subject to `ALLOW_SELF_REGISTER` / `PROVISION_KEY`).

```json
{
  "email": "ward-icu@hospital.example",
  "serial": "MG-1A2B3C",
  "hw_rev": "esp32-devkit-4mb",
  "fw_version": "1.0.0",
  "provision_key": "optional-if-configured"
}
```

Returns `{ ok, device_secret, config, server_time }`. The `device_secret` is
returned **only** during provisioning; the backend stores it encrypted with
AES-256-GCM.

## Device authentication (HMAC)

Every device request except `/health` and `/provision` must send:

```http
X-Device-Email: ward-icu@hospital.example
X-Timestamp: 1760000000
X-Nonce: 1a2b3c4d5e6f7788
X-Signature: <hex hmac>
```

Canonical string (lowercase hex SHA-256):

```
METHOD\nPATH\nTIMESTAMP\nSHA256(body)
```

- `PATH` is the request path **without** the query string.
- `body` is the exact request body (empty string for GET).
- `X-Signature = HMAC-SHA256(device_secret, canonical)`.

The backend accepts timestamps within `HMAC_WINDOW_SEC` (default 300 s) and
rejects reused nonces.

Successful JSON responses are signed over the exact response body:

```http
X-Signature: HMAC-SHA256(device_secret, response_body)
```

The firmware verifies this before applying config.

## `GET /api/v1/devices/:email/config`

HMAC-authenticated. Returns the full device config:

```json
{
  "active_sensors": 4,
  "sensor_labels": ["Sensor 1", "…"],
  "thresholds": [ { "idx": 0, "warn_low": 5, "alarm_low": 2, "warn_high": 35, "alarm_high": 40, "hysteresis": 0.5, "unit": "C" } ],
  "lock_policy": "provision_only",
  "allow_local_override": true,
  "auto_ota": false,
  "grace_days": 7,
  "offline_action": "keep_last",
  "default_active_sensors": 4,
  "quota": { "plan": "basic", "expires_at": "2026-10-09T00:00:00.000Z", "expires_epoch": 1791536000 },
  "firmware": { "version": "1.2.0", "url": "https://…/fw.bin", "sha256": "…", "size": 1251127, "release_notes": "…" },
  "issued_at": 1760000000,
  "expires_at": 1760086400,
  "server_time": 1760000000
}
```

## `POST /api/v1/devices/:email/heartbeat`

HMAC-authenticated.

```json
{
  "fw_version": "1.0.0",
  "active_sensors": 4,
  "wifi_rssi": -58,
  "uptime": 3600,
  "alarm_state": "normal",
  "online": true
}
```

Returns `{ ok, server_time, active_sensors }` (signed).

## `GET /api/v1/firmware/latest?hw=esp32-devkit-4mb&channel=stable`

HMAC-authenticated. Returns `{ firmware, server_time }`; `firmware` may be
`null`.

## Admin UI

Browser-only routes under `/admin` (session cookie `mg_admin`, CSRF protected):

| Route | Purpose |
| --- | --- |
| `GET/POST /admin/login` | Admin login |
| `POST /admin/logout` | Logout |
| `GET /admin` | Dashboard |
| `GET/POST /admin/devices/new` | Register a device |
| `GET/POST /admin/devices/:id` | Edit quota, labels, thresholds, policy, OTA channel |
| `POST /admin/devices/:id/delete` | Delete a device |
| `GET /admin/firmware`, `POST /admin/firmware` | Upload/list firmware |
| `POST /admin/firmware/:id/delete` | Delete firmware |
| `GET /admin/events` | Device event log |

## Error format

```json
{ "error": "human readable message" }
```

Validation errors from the provisioning endpoint may include `details`.
