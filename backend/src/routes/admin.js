import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import express from 'express';
import multer from 'multer';
import rateLimit from 'express-rate-limit';
import { csrfToken, requireAdmin, verifyCsrf } from '../auth.js';
import { encryptSecret, randomHex } from '../crypto.js';
import {
  addEvent,
  createDevice,
  createFirmware,
  deleteFirmware,
  getDeviceById,
  listDevices,
  listEvents,
  listFirmware,
  updateDevice,
  verifyAdmin,
  DEFAULT_LABELS,
  defaultThresholds,
} from '../db.js';

function clampInt(value, min, max, fallback) {
  const n = Number.parseInt(value, 10);
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

function safeJson(value, fallback) {
  try {
    return JSON.parse(value);
  } catch {
    return fallback;
  }
}

function labelsFromBody(body) {
  return Array.from({ length: 8 }, (_, i) => {
    const value = String(body[`label_${i}`] ?? '').trim();
    return (value || `Sensor ${i + 1}`).slice(0, 23);
  });
}

function thresholdsFromBody(body, fallback) {
  return Array.from({ length: 8 }, (_, i) => {
    const base = fallback?.[i] || defaultThresholds()[i];
    return {
      idx: i,
      warn_low: Number(body[`th_warn_low_${i}`] ?? base.warn_low),
      alarm_low: Number(body[`th_alarm_low_${i}`] ?? base.alarm_low),
      warn_high: Number(body[`th_warn_high_${i}`] ?? base.warn_high),
      alarm_high: Number(body[`th_alarm_high_${i}`] ?? base.alarm_high),
      hysteresis: Number(body[`th_hysteresis_${i}`] ?? base.hysteresis),
      unit: String(body[`th_unit_${i}`] ?? base.unit).slice(0, 7),
    };
  });
}

export function adminRoutes(db, config) {
  const router = express.Router();
  const firmwareDir = config.firmwareDir;
  fs.mkdirSync(firmwareDir, { recursive: true });

  const upload = multer({
    dest: path.join(firmwareDir, '.tmp'),
    limits: { fileSize: 4 * 1024 * 1024 },
  });
  fs.mkdirSync(path.join(firmwareDir, '.tmp'), { recursive: true });

  const loginLimiter = rateLimit({
    windowMs: 60 * 1000,
    limit: 8,
    standardHeaders: true,
    legacyHeaders: false,
  });

  router.use((req, res, next) => {
    res.locals.path = req.path;
    next();
  });

  router.get('/login', csrfToken, (req, res) => {
    res.render('login', { title: 'Login Vendor', error: null });
  });

  router.post('/login', loginLimiter, verifyCsrf, (req, res) => {
    const admin = verifyAdmin(db, req.body.username, req.body.password);
    if (!admin) {
      return res.status(401).render('login', {
        title: 'Login Vendor',
        error: 'Username atau password salah.',
      });
    }
    req.session.admin = admin;
    req.session.csrf = randomHex(24);
    addEvent(db, null, 'admin.login', { username: admin.username });
    return res.redirect('/admin');
  });

  router.post('/logout', requireAdmin, verifyCsrf, (req, res) => {
    req.session.destroy(() => res.redirect('/admin/login'));
  });

  router.get('/', requireAdmin, csrfToken, (req, res) => {
    const devices = listDevices(db);
    const now = Date.now();
    for (const d of devices) {
      d.online = d.last_seen ? now - new Date(d.last_seen).getTime() < 10 * 60 * 1000 : false;
    }
    res.render('dashboard', {
      title: 'Devices',
      devices,
      flash: req.session.flash || null,
    });
    delete req.session.flash;
  });

  router.get('/devices/new', requireAdmin, csrfToken, (req, res) => {
    res.render('device_new', { title: 'Tambah Device' });
  });

  router.post('/devices', requireAdmin, verifyCsrf, (req, res) => {
    const email = String(req.body.email || '').trim().toLowerCase();
    const serial = String(req.body.serial || '').trim();
    if (!email.includes('@') || serial.length < 4) {
      req.session.flash = { type: 'error', message: 'Email atau serial tidak valid.' };
      return res.redirect('/admin/devices/new');
    }
    const existing = listDevices(db).find((d) => d.email === email || d.serial === serial);
    if (existing) {
      req.session.flash = { type: 'error', message: 'Email atau serial sudah dipakai.' };
      return res.redirect('/admin/devices/new');
    }
    const device = createDevice(db, {
      email,
      serial,
      hw_rev: String(req.body.hw_rev || 'esp32-devkit-4mb').trim(),
      secret_enc: encryptSecret(randomHex(32), config.masterKey),
      active_sensors: clampInt(req.body.active_sensors, 1, 8, 4),
      sensor_labels: JSON.stringify(DEFAULT_LABELS),
      thresholds: JSON.stringify(defaultThresholds()),
      plan: String(req.body.plan || 'basic').trim(),
      quota_expires_at: req.body.quota_expires_at || null,
      notes: String(req.body.notes || '').slice(0, 500),
    });
    addEvent(db, device, 'admin.device_created', { by: req.session.admin.username });
    req.session.flash = { type: 'ok', message: `Device ${email} ditambahkan.` };
    res.redirect(`/admin/devices/${device.id}`);
  });

  router.get('/devices/:id', requireAdmin, csrfToken, (req, res) => {
    const device = getDeviceById(db, Number(req.params.id));
    if (!device) return res.status(404).send('Device not found');
    res.render('device', {
      title: device.email,
      device,
      flash: req.session.flash || null,
    });
    delete req.session.flash;
  });

  router.post('/devices/:id', requireAdmin, verifyCsrf, (req, res) => {
    const device = getDeviceById(db, Number(req.params.id));
    if (!device) return res.status(404).send('Device not found');

    const labels = labelsFromBody(req.body);
    const thresholds = thresholdsFromBody(req.body, device.thresholds);
    const activeSensors = clampInt(req.body.active_sensors, 1, 8, device.active_sensors);

    const updated = updateDevice(db, device.id, {
      email: String(req.body.email || device.email).trim().toLowerCase(),
      serial: String(req.body.serial || device.serial).trim(),
      hw_rev: String(req.body.hw_rev || device.hw_rev).trim(),
      active_sensors: activeSensors,
      sensor_labels: JSON.stringify(labels),
      thresholds: JSON.stringify(thresholds),
      lock_policy: ['provision_only', 'grace', 'strict'].includes(req.body.lock_policy)
        ? req.body.lock_policy
        : device.lock_policy,
      allow_local_override: req.body.allow_local_override ? 1 : 0,
      grace_days: clampInt(req.body.grace_days, 0, 365, device.grace_days),
      offline_action: ['keep_last', 'fallback_default', 'lock_ui'].includes(
        req.body.offline_action,
      )
        ? req.body.offline_action
        : device.offline_action,
      default_active_sensors: clampInt(
        req.body.default_active_sensors,
        1,
        8,
        device.default_active_sensors,
      ),
      plan: String(req.body.plan || device.plan).trim(),
      quota_expires_at: req.body.quota_expires_at || null,
      fw_channel: ['stable', 'beta', 'dev'].includes(req.body.fw_channel)
        ? req.body.fw_channel
        : device.fw_channel,
      auto_ota: req.body.auto_ota ? 1 : 0,
      notes: String(req.body.notes || '').slice(0, 500),
    });

    addEvent(db, updated, 'admin.device_updated', {
      by: req.session.admin.username,
      active_sensors: updated.active_sensors,
      lock_policy: updated.lock_policy,
    });
    req.session.flash = { type: 'ok', message: 'Konfigurasi device disimpan.' };
    res.redirect(`/admin/devices/${device.id}`);
  });

  router.post('/devices/:id/delete', requireAdmin, verifyCsrf, (req, res) => {
    const device = getDeviceById(db, Number(req.params.id));
    if (device) {
      db.prepare('DELETE FROM devices WHERE id = ?').run(device.id);
      addEvent(db, null, 'admin.device_deleted', { email: device.email });
    }
    req.session.flash = { type: 'ok', message: 'Device dihapus.' };
    res.redirect('/admin');
  });

  router.get('/firmware', requireAdmin, csrfToken, (req, res) => {
    res.render('firmware', {
      title: 'Firmware',
      firmware: listFirmware(db),
      flash: req.session.flash || null,
    });
    delete req.session.flash;
  });

  router.post(
    '/firmware',
    requireAdmin,
    upload.single('firmware'),
    verifyCsrf,
    (req, res) => {
      if (!req.file) {
        req.session.flash = { type: 'error', message: 'File firmware tidak ada.' };
        return res.redirect('/admin/firmware');
      }
      const hwRev = String(req.body.hw_rev || 'esp32-devkit-4mb').trim();
      const version = String(req.body.version || '').trim();
      const channel = ['stable', 'beta', 'dev'].includes(req.body.channel)
        ? req.body.channel
        : 'stable';
      if (!/^\d+\.\d+\.\d+$/.test(version)) {
        fs.unlinkSync(req.file.path);
        req.session.flash = { type: 'error', message: 'Versi harus format x.y.z.' };
        return res.redirect('/admin/firmware');
      }
      const data = fs.readFileSync(req.file.path);
      const sha256 = crypto.createHash('sha256').update(data).digest('hex');
      const filename = `${hwRev}-${version}-${channel}-${sha256.slice(0, 8)}.bin`;
      fs.renameSync(req.file.path, path.join(firmwareDir, filename));
      const row = createFirmware(db, {
        hw_rev: hwRev,
        version,
        channel,
        filename,
        original_name: req.file.originalname,
        sha256,
        size: data.length,
        release_notes: String(req.body.release_notes || '').slice(0, 1000),
      });
      addEvent(db, null, 'admin.firmware_uploaded', {
        by: req.session.admin.username,
        version,
        channel,
        sha256,
      });
      req.session.flash = {
        type: 'ok',
        message: `Firmware ${version} (${channel}) diunggah. SHA-256 ${sha256.slice(0, 16)}...`,
      };
      res.redirect('/admin/firmware');
    },
  );

  router.post('/firmware/:id/delete', requireAdmin, verifyCsrf, (req, res) => {
    const row = deleteFirmware(db, Number(req.params.id));
    if (row) {
      const file = path.join(firmwareDir, row.filename);
      if (fs.existsSync(file)) fs.unlinkSync(file);
    }
    req.session.flash = { type: 'ok', message: 'Firmware dihapus.' };
    res.redirect('/admin/firmware');
  });

  router.get('/events', requireAdmin, csrfToken, (req, res) => {
    res.render('events', {
      title: 'Events',
      events: listEvents(db, 200),
    });
  });

  return router;
}
