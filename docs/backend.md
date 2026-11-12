# Reference Backend

Node.js 20 + Express + SQLite reference implementation for device provisioning,
configuration/quota control, firmware distribution, and an admin UI.

> This backend is a reference/demo. Harden it (TLS termination, secret manager,
> backups, rate limits, audit logging, HA) before production use.

---

## Requirements

- Node.js 20+
- A writable directory for SQLite and firmware files
- Network reachability from the devices (same LAN or a public HTTPS endpoint)

## Install and run

```bash
cd backend
cp .env.example .env
# edit .env (see below)
npm install
npm run seed
npm start
```

Open `http://localhost:3000/admin`.

For development with auto-reload:

```bash
npm run dev
```

## Environment variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `PORT` | `3000` | HTTP port |
| `PUBLIC_BASE_URL` | `http://localhost:3000` | Base URL used in firmware download links |
| `DB_PATH` | `./data/medgas.sqlite` | SQLite database path |
| `FIRMWARE_DIR` | `./data/firmware` | Uploaded firmware storage |
| `MASTER_KEY` | dev value | 32-byte key (64 hex chars) for AES-256-GCM device-secret encryption |
| `ADMIN_USER` | `admin` | Admin bootstrap username (`npm run seed`) |
| `ADMIN_PASS` | `admin123` | Admin bootstrap password |
| `SESSION_SECRET` | dev value | Express session signing secret |
| `ALLOW_SELF_REGISTER` | `true` | Allow unknown emails/serials to provision themselves |
| `PROVISION_KEY` | empty | Shared key required when self-registration is disabled |
| `HMAC_WINDOW_SEC` | `300` | Allowed device clock skew |
| `CONFIG_TTL_SEC` | `86400` | Config cache TTL advertised to devices |

Generate a production master key:

```bash
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

> Changing `MASTER_KEY` makes existing encrypted device secrets unreadable.
> Devices must be re-provisioned.

## Seed data

`npm run seed`:

- Creates/updates the admin from `ADMIN_USER` / `ADMIN_PASS`.
- Creates a demo device:
  - email `demo@medgas.local`
  - serial `MG-DEMO-0001`
  - 4 active sensors, `provision_only`, 7-day grace, `keep_last`
  - quota valid for 365 days
- Device secrets are generated during provisioning, so the seed does not print
  one.

## Database

SQLite tables:

| Table | Contents |
| --- | --- |
| `admins` | Admin users (bcrypt password hashes) |
| `devices` | Email/serial binding, encrypted secret, quota/config, OTA channel |
| `firmware` | Uploaded app images, version, SHA-256, size, channel |
| `events` | Provisioning, config served, heartbeat, OTA, admin events |
| `nonces` | HMAC replay protection |

Useful commands:

```bash
# Backup
sqlite3 data/medgas.sqlite ".backup 'backup.sqlite'"

# Inspect devices
sqlite3 -header -column data/medgas.sqlite \
  "select id,email,serial,active_sensors,lock_policy,plan,quota_expires_at,last_seen from devices;"
```

## Admin UI

### Device management

`/admin/devices/:id` lets you control, **by Email ID**:

- `active_sensors` — vendor quota (1–8)
- `default_active_sensors` — fallback when `offline_action=fallback_default`
- `sensor_labels` — names shown on the device dashboard
- `thresholds` — warn/alarm low/high, hysteresis, unit per channel
- `lock_policy` — `provision_only`, `grace`, `strict`
- `allow_local_override` — whether the local settings page may change policy
- `grace_days` — stale-sync grace window
- `offline_action` — `keep_last`, `fallback_default`, `lock_ui`
- `plan` and `quota_expires_at` — quota metadata and expiry
- `auto_ota` — allow automatic cloud OTA
- `fw_channel` — `stable`, `beta`, etc.

The device applies these on its next cloud sync (default every 6 hours, or when
the user presses **Sync** in the dashboard).

### Quota control by Email ID

1. Find the device by Email ID in `/admin`.
2. Set `active_sensors` (for example 6).
3. Save.
4. The device's next sync returns `active_sensors: 6`.
5. The dashboard immediately hides channels 7–8 and the alarm engine stops
   evaluating them. No reflash is needed.

To force an immediate update, open the device dashboard and press **Sync**, or
wait for the sync interval.

### Firmware management

`/admin/firmware`:

1. Upload the application `.bin`.
2. Set `version`, `hw_rev` (e.g. `esp32-devkit-4mb`), and `channel`.
3. The backend stores the file and computes SHA-256 and size.
4. Devices matching `hw_rev` + `fw_channel` receive it in their config.

The backend serves files from `/firmware/:filename` and uses
`PUBLIC_BASE_URL` when building the manifest URL.

### Events

`/admin/events` shows provisioning attempts, config served, heartbeats, and
admin actions. Useful for diagnosing quota or binding problems.

## Provisioning policy

- `ALLOW_SELF_REGISTER=true` — any valid email/serial can provision. Good for
  demos and lab deployments.
- `ALLOW_SELF_REGISTER=false` — the device must already exist in the admin UI,
  or present the matching `PROVISION_KEY`.

Email and serial are unique and bound together:

- Same email + different serial → HTTP 409
- Same serial + different email → HTTP 409

To move a device to another email, delete or edit the device in the admin UI
first, then factory-reset the device and provision again.

## Security model

| Layer | Mechanism |
| --- | --- |
| Device secret at rest | AES-256-GCM with `MASTER_KEY` |
| Device requests | HMAC-SHA256 over `METHOD\nPATH\nTIMESTAMP\nSHA256(body)` |
| Replay protection | Timestamp window + single-use nonce table |
| Response integrity | `X-Signature` over the exact response body |
| Admin login | bcrypt + Express session + CSRF |
| Admin transport | Helmet, CSP; use HTTPS in production |
| Firmware integrity | SHA-256 in the manifest and on the device |

Notes:

- The device secret is returned in clear only once during provisioning. Protect
  that response (HTTPS).
- Nonces are stored in SQLite; prune old rows periodically in production.
- The reference server trusts `X-Forwarded-For` for client IP logging.
- There is no built-in TLS termination. Put the backend behind a reverse proxy
  (Caddy, nginx, Traefik) with a valid certificate.

## Production checklist

- [ ] Set strong `MASTER_KEY`, `SESSION_SECRET`, and `ADMIN_PASS`.
- [ ] Set `ALLOW_SELF_REGISTER=false` and a strong `PROVISION_KEY`, or
      pre-register devices in the admin UI.
- [ ] Serve the backend over HTTPS and set `PUBLIC_BASE_URL` to the HTTPS URL.
- [ ] Set `NODE_ENV=production` (secure session cookies).
- [ ] Build firmware with `ALLOW_INSECURE_TLS=0` and a pinned root CA/bundle.
- [ ] Change the default AP password prefix, admin password, and OTA password.
- [ ] Back up SQLite and `FIRMWARE_DIR`.
- [ ] Add log rotation, monitoring, and nonce cleanup.
- [ ] Restrict `/admin` by network/VPN where possible.
- [ ] Review the alarm thresholds and sensor drivers with a safety engineer.

## Tests

```bash
cd backend
npm test
```

Tests use Vitest + Supertest and run against a temporary database.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Device gets HTTP 401 | Clock skew, wrong secret, missing headers, or reused nonce. Check server logs. |
| Device gets HTTP 409 on provision | Email/serial already bound. Edit/delete the device in the admin UI. |
| Device never receives config | `PUBLIC_BASE_URL`/`api_base` reachability, firewall, backend logs. |
| `invalid signature` on device | HMAC path must exclude the query string; body must be byte-exact. |
| Firmware URL unreachable | `PUBLIC_BASE_URL` must be reachable from the device; `/firmware` is public. |
| Admin login fails after changing `.env` | Run `npm run seed` again; existing admin passwords are not overwritten by env changes. |
| Device secrets unreadable | `MASTER_KEY` changed. Re-provision devices. |
