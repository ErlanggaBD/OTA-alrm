# OTA Updates

The project supports three OTA paths:

1. **Arduino IDE network OTA** — only for the Arduino firmware.
2. **Local firmware upload** — browser upload through the dashboard, available in
   both firmwares.
3. **Cloud OTA** — vendor publishes a firmware record in the backend; the device
   downloads it during cloud sync.

All paths write to the inactive OTA partition and only switch the boot partition
after the image is accepted. A power loss during upload leaves the current
firmware bootable.

---

## 1. Arduino IDE network OTA

Requirements:

- The board was flashed with an OTA-capable partition scheme. This project uses
  `PartitionScheme=min_spiffs` (two 1.875 MB app slots).
- The device and your computer are on the same network.
- `ArduinoOTA` is initialized in `ota_manager.cpp` after Wi-Fi connects.

The advertised hostname is:

```
medgas-<serial>        e.g. medgas-MG-1A2B3C
```

The OTA password is `g_config.ota_pass`. Default:

```c
#define DEFAULT_OTA_PASS "medgasota"
```

### Steps

1. Open the sketch in Arduino IDE.
2. Select **Tools → Board → ESP32 Dev Module**.
3. Select **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP x2)**.
4. Wait for the device to join Wi-Fi and print `OTA Arduino IDE aktif`.
5. Select **Tools → Port → medgas-MG-XXXXXX (network)**.
6. Click **Upload**. Enter the OTA password if prompted.

With `arduino-cli`, list network ports and upload:

```bash
arduino-cli board list
arduino-cli upload -p medgas-MG-1A2B3C \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  firmware-arduino/MedGasAlarm
```

> Arduino IDE OTA is not available in the ESP-IDF build.

---

## 2. Local firmware upload (dashboard)

Both firmwares expose:

```http
POST /api/ota
Content-Type: application/octet-stream
X-CSRF-Token: <session csrf>
X-Firmware-SHA256: <optional 64 hex chars>
```

The browser:

1. Reads the selected `.bin` file.
2. Computes SHA-256 in JavaScript (`crypto.subtle`).
3. Sends the raw bytes with the hash header.
4. Shows upload progress and waits for the device to reboot.

The device:

1. Validates the session, CSRF token, and lock policy.
2. Starts an OTA update on the inactive partition.
3. Streams chunks into flash.
4. Verifies SHA-256 (Arduino: `Update.setSHA256`; ESP-IDF: streaming
   `mbedtls_sha256`).
5. Sets the new boot partition and reboots after ~3 seconds.

Status is visible through:

```http
GET /api/ota/status
```

```json
{ "status": "idle|uploading|verifying|success|error", "progress": 0, "error": "", "reboot_at": 0 }
```

### Building a `.bin`

Arduino:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --output-dir build \
  firmware-arduino/MedGasAlarm
# upload build/MedGasAlarm.ino.bin
```

ESP-IDF:

```bash
cd firmware-esp-idf
idf.py build
# upload build/medgas_alarm.bin
```

> Do not upload the merged factory image or the bootloader/partition table for
> app-only OTA. Use the application `.bin`.

---

## 3. Cloud OTA

The backend stores firmware records per `hw_rev` and `channel` (`stable`,
`beta`, ...). The device receives the newest matching firmware in its config
payload:

```json
"firmware": {
  "version": "1.2.0",
  "url": "https://vendor.example/firmware/medgas_alarm-1.2.0.bin",
  "sha256": "…64 hex…",
  "size": 1251127,
  "release_notes": "…"
}
```

The device stores it in `DeviceConfig` when the version differs from
`FW_VERSION`.

### Automatic OTA

Automatic cloud OTA is enabled per device by `auto_ota`:

- Backend device record → `auto_ota`.
- Firmware: `g_config.auto_ota` (default `0`).
- When enabled, the device attempts the update once per boot after Wi-Fi is up.

### Manual cloud OTA

Dashboard → Settings → **Update from cloud**:

```http
POST /api/ota/cloud
```

The device downloads in a background task, verifies size and SHA-256, then sets
the boot partition and reboots.

### Publishing firmware in the backend

1. Log in to `/admin`.
2. Open **Firmware**.
3. Upload the app `.bin`, set `version`, `hw_rev`, and `channel`.
4. The backend computes the SHA-256 and stores the file under `FIRMWARE_DIR`.
5. Devices with a matching `hw_rev` and channel receive it on the next sync.

---

## Partitioning and rollback

| Build | OTA slots | App size | Extra |
| --- | --- | --- | --- |
| Arduino `min_spiffs` | `app0` + `app1` | 1.875 MB each | ~190 KB SPIFFS |
| ESP-IDF custom | `ota_0` + `ota_1` | 1.875 MB each | 128 KB SPIFFS |

The ESP-IDF bootloader keeps the previous slot until the new image boots
successfully. If the new image crashes before confirming, the bootloader can
roll back (subject to `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). Enable rollback
in `menuconfig` for production.

The Arduino core performs its own image validation in `Update.end()`.

---

## Security

- Set `ALLOW_INSECURE_TLS=0` for HTTPS OTA and configure a root CA or the
  ESP-IDF certificate bundle.
- Change `DEFAULT_OTA_PASS` before deployment; the Arduino IDE OTA password is
  stored in NVS config.
- Always publish a SHA-256 with cloud firmware. The device rejects mismatches.
- Restrict dashboard OTA to authenticated admin sessions; CSRF is required.
- Serve firmware over HTTPS in production. The demo backend uses HTTP.

---

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Network port not listed | Device not on Wi-Fi, mDNS blocked, or not provisioned. Check serial log. |
| `OTA lain sedang berjalan` | Another OTA (cloud or web) is active. Wait for it to finish. |
| `SHA-256 firmware tidak cocok` | Wrong `.bin` or corrupted download. Rebuild/re-upload. |
| `Tidak ada partisi OTA tersedia` | Wrong partition scheme (Arduino) or partition table (ESP-IDF). |
| Upload succeeds but version unchanged | Uploaded bootloader/partition image instead of the app `.bin`, or boot partition switch failed. |
| Device reboots into old firmware | New image failed validation/rollback; check serial logs. |
| SPIFFS assets missing after ESP-IDF OTA | App OTA does not update SPIFFS. Flash the full image (`idf.py flash`) when the UI changes. |
