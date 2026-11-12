# Provisioning and Captive Portal

## Overview

Provisioning is the only time the device accepts an Email ID and Wi-Fi
credentials from a browser. The identity is **not** written to NVS until all of
these succeed:

1. The device joins the requested Wi-Fi network.
2. It obtains an IP address.
3. It reaches the vendor backend at `api_base`.
4. `POST /api/v1/provision` returns `ok: true` and a `device_secret`.
5. The returned config is applied.

If any step fails, the device stays in the portal and keeps no partial identity.

## Access point

The SoftAP credentials are derived from the Wi-Fi STA MAC:

| Field | Format | Example |
| --- | --- | --- |
| SSID | `MedGasAlarm-XXXX` | `MedGasAlarm-1A2B` |
| Password | `medgas-XXXX` | `medgas-1A2B` |

`XXXX` is the last two MAC bytes. The same values are printed on the serial
console at 115200 baud:

```
----------------------------------------------
 AP SSID  : MedGasAlarm-1A2B
 AP PASS  : medgas-1A2B
 portal   : http://192.168.4.1/
 mode     : registrasi awal
----------------------------------------------
```

The portal IP is `192.168.4.1` on ESP-IDF and the default SoftAP IP on Arduino.

## Captive portal

Both firmwares run a minimal DNS responder that answers every A query with the
device's AP IP. Unknown HTTP paths redirect to `/` while in AP mode. This makes
phones and laptops open the setup page automatically.

The setup page is `shared/web/setup.html` and is served as:

- Arduino: embedded `SETUP_HTML` from `web_ui.h`.
- ESP-IDF: `/spiffs/setup.html`.

## Setup form fields

| Field | JSON key | Notes |
| --- | --- | --- |
| Email ID | `email` | Must contain `@`; bound to the device serial. |
| Wi-Fi SSID | `ssid` | 1–32 chars. Can be typed or chosen from scan. |
| Wi-Fi password | `password` | Empty allowed for open networks. |
| Admin password | `admin_password` | Min. 8 chars on first boot. |
| Server URL | `api_base` | Optional; defaults to `DEFAULT_API_BASE`. Must start with `http://` or `https://`. |

## Flow

```
Browser                Device                     Backend
  │  POST /api/provision  │                          │
  │──────────────────────►│                          │
  │                       │ join Wi-Fi               │
  │                       │─────────────────────────►│
  │                       │ POST /api/v1/provision   │
  │                       │─────────────────────────►│
  │                       │ {ok, device_secret,      │
  │                       │  config, server_time}    │
  │                       │◄─────────────────────────│
  │                       │ save NVS, apply config   │
  │  {ok, message}        │                          │
  │◄──────────────────────│                          │
  │                       │ reboot in ~3 s           │
```

### Request

```http
POST /api/v1/provision
Content-Type: application/json

{
  "email": "ward-icu@hospital.example",
  "serial": "MG-1A2B3C",
  "hw_rev": "esp32-devkit-4mb",
  "fw_version": "1.0.0"
}
```

`serial` is generated as `MG-` plus the last three MAC bytes. The backend binds
one serial to one email. If the email already exists with a different serial, or
the serial belongs to another email, provisioning is rejected with HTTP 409.

### Response

```json
{
  "ok": true,
  "device_secret": "64-hex-characters",
  "config": {
    "active_sensors": 4,
    "sensor_labels": ["Sensor 1", "..."],
    "thresholds": [{ "idx": 0, "warn_low": 5, "alarm_low": 2, "warn_high": 35, "alarm_high": 40, "hysteresis": 0.5, "unit": "C" }],
    "lock_policy": "provision_only",
    "allow_local_override": true,
    "auto_ota": false,
    "grace_days": 7,
    "offline_action": "keep_last",
    "default_active_sensors": 4,
    "quota": { "plan": "basic", "expires_at": null, "expires_epoch": 0 },
    "firmware": null,
    "issued_at": 1760000000,
    "expires_at": 1760086400,
    "server_time": 1760000000
  },
  "server_time": 1760000000
}
```

The device stores `email`, `ssid`, `wifi_pass`, `api_base`, `device_secret`,
`serial`, the admin password hash, and the applied config in NVS.

## Recovery portal

If a provisioned device cannot connect to Wi-Fi or reach the server at boot, it
opens the same AP/portal in **recovery** mode. The setup page shows the stored
Email ID (read-only in recovery) and asks for the admin password before allowing
changes. On success it updates Wi-Fi/server settings and reboots.

Recovery does **not** erase the device secret or the quota. It is intended to fix
Wi-Fi credentials or a changed server address.

## Factory reset

Two ways:

1. Hold the `BOOT` button (GPIO0) for 8 seconds.
2. Dashboard → Settings → Factory reset (type `RESET` to confirm).

Both erase the NVS configuration. The device reboots into the first-boot portal.
Alarm outputs are reset to safe defaults during the reboot.

## Changing defaults

Edit `app_config.h` in the relevant firmware folder:

```c
#define DEFAULT_API_BASE "http://192.168.1.10:3000"
#define DEFAULT_ADMIN_PASS "admin12345"
#define AP_PREFIX "MedGasAlarm-"
#define AP_PASS_PREFIX "medgas-"
```

The backend seed credentials are separate (`backend/.env` and `npm run seed`).

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Portal does not open automatically | Browse to `http://192.168.4.1/` manually. |
| "Gagal terhubung ke Wi-Fi" | 2.4 GHz network? ESP32 does not support 5 GHz. Check password. |
| "Tidak bisa menghubungi server" | Device must reach `api_base`; check IP, port, firewall, and that the backend is running. |
| HTTP 409 during provisioning | Email/serial already bound differently; use the admin UI to reset the binding. |
| Portal loops after successful setup | Backend returned `ok` but the device could not save NVS; check flash/NVS errors on serial. |
| Provisioned device always opens recovery | Wi-Fi changed or server unreachable; use recovery to update. |
