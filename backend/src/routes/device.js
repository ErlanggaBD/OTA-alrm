import express from 'express';
import { z } from 'zod';
import { deviceAuth, signedJson } from '../auth.js';
import { encryptSecret, randomHex } from '../crypto.js';
import {
  addEvent,
  createDevice,
  getDeviceByEmail,
  getDeviceBySerial,
  latestFirmware,
  updateDevice,
} from '../db.js';

function nowSec() {
  return Math.floor(Date.now() / 1000);
}

function clientIp(req) {
  return String(req.headers['x-forwarded-for'] || req.socket.remoteAddress || '')
    .split(',')[0]
    .trim();
}

function firmwareManifest(config, fw) {
  if (!fw) return null;
  return {
    version: fw.version,
    url: `${config.publicBaseUrl}/firmware/${encodeURIComponent(fw.filename)}`,
    sha256: fw.sha256,
    size: fw.size,
    release_notes: fw.release_notes || '',
  };
}

export function buildConfig(config, db, device) {
  const now = nowSec();
  const fw = latestFirmware(db, device.hw_rev, device.fw_channel);
  return {
    active_sensors: device.active_sensors,
    sensor_labels: device.sensor_labels,
    thresholds: device.thresholds,
    lock_policy: device.lock_policy,
    allow_local_override: device.allow_local_override,
    auto_ota: device.auto_ota,
    grace_days: device.grace_days,
    offline_action: device.offline_action,
    default_active_sensors: device.default_active_sensors,
    quota: {
      plan: device.plan,
      expires_at: device.quota_expires_at,
      expires_epoch: device.quota_expires_at
        ? Math.floor(new Date(device.quota_expires_at).getTime() / 1000)
        : 0,
    },
    firmware: firmwareManifest(config, fw),
    issued_at: now,
    expires_at: now + config.configTtlSec,
    server_time: now,
  };
}

export function deviceRoutes(db, config) {
  const router = express.Router();

  router.get('/health', (req, res) => {
    res.json({ ok: true, service: 'medgas-alarm-backend', server_time: nowSec() });
  });

  const provisionSchema = z.object({
    email: z.string().email().max(96),
    serial: z.string().min(4).max(64),
    hw_rev: z.string().max(64).optional().default('esp32-devkit-4mb'),
    fw_version: z.string().max(32).optional().default('0.0.0'),
    provision_key: z.string().max(128).optional(),
  });

  router.post('/provision', (req, res) => {
    const parsed = provisionSchema.safeParse(req.body);
    if (!parsed.success) {
      return res.status(400).json({ error: 'invalid request', details: parsed.error.flatten() });
    }
    const { email, serial, hw_rev, fw_version, provision_key } = parsed.data;

    let device = getDeviceByEmail(db, email);
    if (device && device.serial !== serial) {
      addEvent(db, device, 'provision.rejected', { reason: 'serial_mismatch', serial });
      return res.status(409).json({ error: 'email is already bound to another serial' });
    }

    const serialOwner = getDeviceBySerial(db, serial);
    if (serialOwner && serialOwner.email !== email) {
      addEvent(db, serialOwner, 'provision.rejected', { reason: 'serial_in_use', email });
      return res.status(409).json({ error: 'serial is already bound to another email' });
    }

    const plainSecret = randomHex(32);
    if (!device) {
      if (!config.allowSelfRegister) {
        const keyOk = config.provisionKey && provision_key === config.provisionKey;
        if (!keyOk) {
          return res.status(403).json({
            error: 'device is not registered; ask the vendor to add it in the admin UI',
          });
        }
      }
      device = createDevice(db, {
        email,
        serial,
        hw_rev,
        secret_enc: encryptSecret(plainSecret, config.masterKey),
        notes: `Provisioned by device on ${new Date().toISOString()} (fw ${fw_version})`,
      });
      addEvent(db, device, 'device.created', { serial, hw_rev, fw_version });
    } else {
      device = updateDevice(db, device.id, {
        secret_enc: encryptSecret(plainSecret, config.masterKey),
        hw_rev,
      });
      addEvent(db, device, 'device.reprovisioned', { fw_version });
    }

    device = updateDevice(db, device.id, {
      last_seen: new Date().toISOString(),
      last_ip: clientIp(req),
    });

    return res.json({
      ok: true,
      device_secret: plainSecret,
      config: buildConfig(config, db, device),
      server_time: nowSec(),
    });
  });

  const auth = deviceAuth(db, config);

  router.get('/devices/:email/config', auth, (req, res) => {
    const device = updateDevice(db, req.device.id, {
      last_seen: new Date().toISOString(),
      last_ip: clientIp(req),
    });
    const payload = buildConfig(config, db, device);
    addEvent(db, device, 'config.served', {
      active_sensors: payload.active_sensors,
      firmware: payload.firmware?.version || null,
    });
    return signedJson(res, payload, req.deviceSecret);
  });

  router.post('/devices/:email/heartbeat', auth, (req, res) => {
    const body = req.body || {};
    const device = updateDevice(db, req.device.id, {
      last_seen: new Date().toISOString(),
      last_ip: clientIp(req),
    });
    addEvent(db, device, 'device.heartbeat', {
      fw_version: body.fw_version || null,
      active_sensors: body.active_sensors ?? null,
      wifi_rssi: body.wifi_rssi ?? null,
      uptime: body.uptime ?? null,
      alarm_state: body.alarm_state ?? null,
      online: body.online ?? null,
    });
    return signedJson(
      res,
      { ok: true, server_time: nowSec(), active_sensors: device.active_sensors },
      req.deviceSecret,
    );
  });


  router.get('/firmware/latest', auth, (req, res) => {
    const hw = String(req.query.hw || req.device.hw_rev);
    const channel = String(req.query.channel || req.device.fw_channel || 'stable');
    const fw = latestFirmware(db, hw, channel);
    return signedJson(
      res,
      { firmware: firmwareManifest(config, fw), server_time: nowSec() },
      req.deviceSecret,
    );
  });

  return router;
}
