# MedGasAlarm — ESP32 Medical Gas Alarm (OTA + Cloud Quota)

Firmware and reference backend for an 8-channel ESP32 medical gas / environmental
alarm. The system supports first-boot Wi-Fi provisioning, a local dashboard, cloud
sync, remote sensor-quota control by Email ID, and OTA updates.

Two complete firmware implementations are provided:

| Folder | Toolchain | Web UI delivery |
| --- | --- | --- |
| `firmware-arduino/` | Arduino IDE / `arduino-cli` (ESP32 core 3.x) | Embedded in flash as `web_ui.h` |
| `firmware-esp-idf/` | ESP-IDF 5.x (`idf.py`) | SPIFFS `storage` partition |

Both share the same vanilla HTML/CSS/JS dashboard from `shared/web/`.

---

## Features

- **First-boot captive portal** — collects Email ID, Wi-Fi SSID/password, admin
  password, and the vendor server URL. Nothing is persisted until the device
  reaches the internet, provisions with the backend, and receives a device secret.
- **Recovery portal** — if a provisioned device cannot join Wi-Fi or reach the
  server, it re-opens the same captive portal without losing its identity.
- **Local dashboard** — live sensor values, alarm state, thresholds, logs,
  manual cloud sync, local firmware upload, cloud OTA, reboot, factory reset.
- **Dynamic sensor count** — hardware has 8 channels, but only the active count
  (e.g. 4 or 6) is read, alarmed, and shown. Inactive cards/functions are hidden.
- **Vendor quota control by Email ID** — the backend returns `active_sensors`,
  `sensor_labels`, `thresholds`, `lock_policy`, `offline_action`, and
  `quota.expires_epoch`. The device applies them on every sync.
- **Runtime lock policy** — `provision_only`, `grace`, or `strict`, changeable
  from the cloud or locally (when `allow_local_override` is enabled).
- **OTA** — Arduino IDE network OTA, local `.bin` upload through the dashboard,
  and vendor-triggered cloud OTA. Firmware can be SHA-256 verified.
- **Offline-safe alarm engine** — sensors, buzzer, relay, and LED always run
  locally. Internet loss never disables alarm outputs.
- **Reference backend** — Node.js 20 + Express + SQLite + EJS admin UI, HMAC
  device authentication, AES-256-GCM secret storage, quota and firmware management.

---

## Repository layout

```
.
├── backend/                 Node.js reference server + admin UI
├── firmware-arduino/        Arduino IDE firmware
│   └── MedGasAlarm/
│       ├── MedGasAlarm.ino
│       ├── app_config.h
│       ├── web_ui.h         generated from shared/web
│       └── ... modules
├── firmware-esp-idf/        ESP-IDF firmware
│   ├── main/
│   ├── spiffs/              generated from shared/web
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── shared/web/              shared dashboard assets
│   ├── build_embed.py       regenerates Arduino web_ui.h
│   └── ...
└── docs/
```

---

## Hardware

Target board: **ESP32 DevKit (4 MB flash)**.

Example sensor: 8 × DHT22 (demo only — replace the sensor driver with the real
medical gas probe driver for production).

| Function | GPIO |
| --- | --- |
| Sensor 1..8 data | 21, 22, 23, 25, 26, 27, 32, 33 |
| Buzzer | 4 |
| Relay | 5 (active-low by default) |
| Status LED | 2 |
| Reset / factory-reset button | 0 (`BOOT`, active-low, hold 8 s) |

See [`docs/hardware.md`](docs/hardware.md) for wiring, power, and sensor
replacement notes.

---

## Quick start

### 1. Backend

```bash
cd backend
cp .env.example .env          # edit MASTER_KEY, SESSION_SECRET, PUBLIC_BASE_URL
npm install
npm run seed                  # admin/admin123 + demo device
npm start                     # http://localhost:3000/admin
npm test                      # integration tests (Vitest + Supertest, temp DB)
```

Default seed credentials (change immediately):

- Admin UI: `admin` / `admin123`
- Demo device: `demo@medgas.local`, serial `MG-DEMO-0001`

Point the firmware at a reachable backend address. The compile-time default is
`http://192.168.1.10:3000` (`DEFAULT_API_BASE` in each `app_config.h`). You can
also enter the server URL in the captive portal.

### 2. Arduino firmware

```bash
# Required libraries (Library Manager or arduino-cli)
arduino-cli lib install ArduinoJson
arduino-cli lib install "DHT sensor library"
arduino-cli lib install "Adafruit Unified Sensor"

# Compile for ESP32 Dev Module, 4 MB, minimal SPIFFS
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  firmware-arduino/MedGasAlarm

# Flash
arduino-cli upload -p /dev/ttyUSB0 \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  firmware-arduino/MedGasAlarm
```

After editing anything in `shared/web/`, regenerate the embedded UI:

```bash
python3 shared/web/build_embed.py
```

### 3. ESP-IDF firmware

```bash
cd firmware-esp-idf
python3 tools/sync_web.py          # copy shared/web -> spiffs/
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The `storage` SPIFFS image is built and flashed automatically because
`main/CMakeLists.txt` uses `spiffs_create_partition_image(... FLASH_IN_PROJECT)`.

### 4. First boot

1. Power the board. It starts AP `MedGasAlarm-XXXX` with password
   `medgas-XXXX` (derived from the MAC; also printed on serial).
2. Join the AP and open `http://192.168.4.1/`.
3. Enter Email ID, choose/scan Wi-Fi, enter Wi-Fi password, admin password
   (min. 8 chars), and optionally the server URL.
4. The device connects to Wi-Fi, calls `POST /api/v1/provision`, saves the
   returned device secret/config to NVS, then reboots to the dashboard.
5. Open `http://<device-ip>/` and log in with the admin password.

Default AP prefix/password and the default admin password are compile-time
constants in `app_config.h`. Change them before deploying.

---

## Partition layout

### Arduino (`PartitionScheme=min_spiffs`)

| Partition | Size |
| --- | --- |
| `app0` | 1.875 MB |
| `app1` | 1.875 MB |
| SPIFFS | ~190 KB |

The Arduino build embeds the UI in `web_ui.h`; SPIFFS is not required by the
Arduino firmware.

### ESP-IDF (`firmware-esp-idf/partitions.csv`)

| Label | Offset | Size |
| --- | --- | --- |
| `nvs` | `0x9000` | 24 KB |
| `otadata` | `0xF000` | 8 KB |
| `phy_init` | `0x11000` | 4 KB |
| `ota_0` | `0x20000` | 1.875 MB |
| `ota_1` | `0x200000` | 1.875 MB |
| `storage` (SPIFFS) | `0x3E0000` | 128 KB |

---

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — modules, state, concurrency,
  lock policy, data flow.
- [`docs/provisioning.md`](docs/provisioning.md) — captive portal and first-boot
  state machine.
- [`docs/ota.md`](docs/ota.md) — Arduino IDE, local upload, and cloud OTA.
- [`docs/api.md`](docs/api.md) — local device API and vendor backend API.
- [`docs/backend.md`](docs/backend.md) — backend setup, quota control, firmware
  management, security.
- [`docs/hardware.md`](docs/hardware.md) — wiring and sensor replacement.

---

## Security notes (read before production)

- `ALLOW_INSECURE_TLS=1` is the demo default. Set it to `0` and configure the
  root CA / ESP-IDF certificate bundle before using HTTPS in production.
- Change `MASTER_KEY`, `SESSION_SECRET`, `ADMIN_PASS`, the default admin
  password, the AP password prefix, and the default OTA password.
- The backend stores device secrets encrypted with AES-256-GCM, but the device
  secret itself is returned once during provisioning and stored in NVS. Protect
  physical and serial access to the device.
- The alarm engine is intentionally independent of the cloud. A strict lock
  policy only locks the **dashboard**, never the buzzer/relay outputs.
- DHT22 is a demonstration sensor. Real medical gas sensors require appropriate
  calibration, fault detection, and safety certification.

---

## License

Add your project license here before distribution.
# OTA-alrm
