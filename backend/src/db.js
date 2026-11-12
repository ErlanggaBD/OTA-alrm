import Database from 'better-sqlite3';
import bcrypt from 'bcryptjs';
import { encryptSecret, randomHex } from './crypto.js';

export const DEFAULT_LABELS = Array.from({ length: 8 }, (_, i) => `Sensor ${i + 1}`);

export function defaultThresholds() {
  return Array.from({ length: 8 }, (_, idx) => ({
    idx,
    warn_low: 5,
    alarm_low: 2,
    warn_high: 35,
    alarm_high: 40,
    hysteresis: 0.5,
    unit: 'C',
  }));
}

function rowToDevice(row) {
  if (!row) return null;
  return {
    ...row,
    active_sensors: Number(row.active_sensors),
    default_active_sensors: Number(row.default_active_sensors),
    grace_days: Number(row.grace_days),
    allow_local_override: Boolean(row.allow_local_override),
    auto_ota: Boolean(row.auto_ota),
    sensor_labels: JSON.parse(row.sensor_labels),
    thresholds: JSON.parse(row.thresholds),
  };
}

export function openDb(config) {
  const db = new Database(config.dbPath);
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');
  initSchema(db);
  return db;
}

export function initSchema(db) {
  db.exec(`
    CREATE TABLE IF NOT EXISTS admins (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      username TEXT UNIQUE NOT NULL,
      password_hash TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT (datetime('now'))
    );

    CREATE TABLE IF NOT EXISTS devices (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      email TEXT UNIQUE NOT NULL,
      serial TEXT UNIQUE NOT NULL,
      hw_rev TEXT NOT NULL DEFAULT 'esp32-devkit-4mb',
      secret_enc TEXT NOT NULL,
      active_sensors INTEGER NOT NULL DEFAULT 4,
      sensor_labels TEXT NOT NULL,
      thresholds TEXT NOT NULL,
      lock_policy TEXT NOT NULL DEFAULT 'provision_only',
      allow_local_override INTEGER NOT NULL DEFAULT 1,
      grace_days INTEGER NOT NULL DEFAULT 7,
      offline_action TEXT NOT NULL DEFAULT 'keep_last',
      default_active_sensors INTEGER NOT NULL DEFAULT 4,
      plan TEXT NOT NULL DEFAULT 'basic',
      quota_expires_at TEXT,
      fw_channel TEXT NOT NULL DEFAULT 'stable',
      auto_ota INTEGER NOT NULL DEFAULT 0,
      last_seen TEXT,
      last_ip TEXT,
      notes TEXT,
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at TEXT NOT NULL DEFAULT (datetime('now'))
    );

    CREATE TABLE IF NOT EXISTS firmware (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      hw_rev TEXT NOT NULL,
      version TEXT NOT NULL,
      channel TEXT NOT NULL DEFAULT 'stable',
      filename TEXT NOT NULL,
      original_name TEXT,
      sha256 TEXT NOT NULL,
      size INTEGER NOT NULL,
      release_notes TEXT,
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      UNIQUE(hw_rev, version, channel)
    );

    CREATE TABLE IF NOT EXISTS events (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id INTEGER,
      email TEXT,
      type TEXT NOT NULL,
      payload TEXT,
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      FOREIGN KEY(device_id) REFERENCES devices(id) ON DELETE SET NULL
    );

    CREATE TABLE IF NOT EXISTS nonces (
      nonce TEXT PRIMARY KEY,
      device_id INTEGER NOT NULL,
      created_at INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_events_device ON events(device_id, id DESC);
    CREATE INDEX IF NOT EXISTS idx_events_created ON events(created_at DESC);
    CREATE INDEX IF NOT EXISTS idx_nonces_created ON nonces(created_at);
  `);
}

export function ensureAdmin(db, username, password) {
  const existing = db.prepare('SELECT id FROM admins WHERE username = ?').get(username);
  if (existing) return existing.id;
  const hash = bcrypt.hashSync(password, 12);
  const info = db
    .prepare('INSERT INTO admins (username, password_hash) VALUES (?, ?)')
    .run(username, hash);
  return Number(info.lastInsertRowid);
}

export function verifyAdmin(db, username, password) {
  const row = db.prepare('SELECT * FROM admins WHERE username = ?').get(username);
  if (!row) return null;
  if (!bcrypt.compareSync(password, row.password_hash)) return null;
  return { id: row.id, username: row.username };
}

export function createDevice(db, data) {
  const info = db
    .prepare(
      `INSERT INTO devices
        (email, serial, hw_rev, secret_enc, active_sensors, sensor_labels, thresholds,
         lock_policy, allow_local_override, grace_days, offline_action, default_active_sensors,
         plan, quota_expires_at, fw_channel, auto_ota, notes)
       VALUES
        (@email, @serial, @hw_rev, @secret_enc, @active_sensors, @sensor_labels, @thresholds,
         @lock_policy, @allow_local_override, @grace_days, @offline_action, @default_active_sensors,
         @plan, @quota_expires_at, @fw_channel, @auto_ota, @notes)`,
    )
    .run({
      hw_rev: 'esp32-devkit-4mb',
      active_sensors: 4,
      sensor_labels: JSON.stringify(DEFAULT_LABELS),
      thresholds: JSON.stringify(defaultThresholds()),
      lock_policy: 'provision_only',
      allow_local_override: 1,
      grace_days: 7,
      offline_action: 'keep_last',
      default_active_sensors: 4,
      plan: 'basic',
      quota_expires_at: null,
      fw_channel: 'stable',
      auto_ota: 0,
      notes: null,
      ...data,
    });
  return getDeviceById(db, Number(info.lastInsertRowid));
}

export function updateDevice(db, id, data) {
  const allowed = [
    'email',
    'serial',
    'hw_rev',
    'secret_enc',
    'active_sensors',
    'sensor_labels',
    'thresholds',
    'lock_policy',
    'allow_local_override',
    'grace_days',
    'offline_action',
    'default_active_sensors',
    'plan',
    'quota_expires_at',
    'fw_channel',
    'auto_ota',
    'notes',
    'last_seen',
    'last_ip',
  ];
  const sets = [];
  const values = {};
  for (const key of allowed) {
    if (data[key] !== undefined) {
      sets.push(`${key} = @${key}`);
      values[key] = data[key];
    }
  }
  if (!sets.length) return getDeviceById(db, id);
  sets.push(`updated_at = datetime('now')`);
  values.id = id;
  db.prepare(`UPDATE devices SET ${sets.join(', ')} WHERE id = @id`).run(values);
  return getDeviceById(db, id);
}

export function getDeviceById(db, id) {
  return rowToDevice(db.prepare('SELECT * FROM devices WHERE id = ?').get(id));
}

export function getDeviceByEmail(db, email) {
  return rowToDevice(db.prepare('SELECT * FROM devices WHERE email = ?').get(email));
}

export function getDeviceBySerial(db, serial) {
  return rowToDevice(db.prepare('SELECT * FROM devices WHERE serial = ?').get(serial));
}

export function listDevices(db) {
  return db.prepare('SELECT * FROM devices ORDER BY id DESC').all().map(rowToDevice);
}

export function createFirmware(db, data) {
  const info = db
    .prepare(
      `INSERT INTO firmware
        (hw_rev, version, channel, filename, original_name, sha256, size, release_notes)
       VALUES (@hw_rev, @version, @channel, @filename, @original_name, @sha256, @size, @release_notes)
       ON CONFLICT(hw_rev, version, channel) DO UPDATE SET
         filename = excluded.filename,
         original_name = excluded.original_name,
         sha256 = excluded.sha256,
         size = excluded.size,
         release_notes = excluded.release_notes`,
    )
    .run(data);
  return db
    .prepare('SELECT * FROM firmware WHERE hw_rev = ? AND version = ? AND channel = ?')
    .get(data.hw_rev, data.version, data.channel);
}

export function latestFirmware(db, hwRev, channel = 'stable') {
  return (
    db
      .prepare(
        'SELECT * FROM firmware WHERE hw_rev = ? AND channel = ? ORDER BY id DESC LIMIT 1',
      )
      .get(hwRev, channel) || null
  );
}

export function listFirmware(db) {
  return db.prepare('SELECT * FROM firmware ORDER BY id DESC').all();
}

export function deleteFirmware(db, id) {
  const row = db.prepare('SELECT * FROM firmware WHERE id = ?').get(id);
  if (row) db.prepare('DELETE FROM firmware WHERE id = ?').run(id);
  return row;
}

export function addEvent(db, device, type, payload) {
  db.prepare(
    'INSERT INTO events (device_id, email, type, payload) VALUES (?, ?, ?, ?)',
  ).run(
    device?.id ?? null,
    device?.email ?? null,
    type,
    payload === undefined ? null : JSON.stringify(payload),
  );
}

export function listEvents(db, limit = 100) {
  return db
    .prepare('SELECT * FROM events ORDER BY id DESC LIMIT ?')
    .all(limit)
    .map((e) => ({ ...e, payload: e.payload ? JSON.parse(e.payload) : null }));
}

export function useNonce(db, nonce, deviceId, nowSec, windowSec) {
  db.prepare('DELETE FROM nonces WHERE created_at < ?').run(nowSec - windowSec * 2);
  try {
    db.prepare('INSERT INTO nonces (nonce, device_id, created_at) VALUES (?, ?, ?)').run(
      nonce,
      deviceId,
      nowSec,
    );
    return true;
  } catch {
    return false;
  }
}

export function seedDemo(db, config) {
  ensureAdmin(db, config.adminUser, config.adminPass);
  let device = getDeviceByEmail(db, 'demo@medgas.local');
  if (!device) {
    device = createDevice(db, {
      email: 'demo@medgas.local',
      serial: 'MG-DEMO-0001',
      hw_rev: 'esp32-devkit-4mb',
      secret_enc: encryptSecret(randomHex(32), config.masterKey),
      active_sensors: 4,
      plan: 'demo',
      quota_expires_at: new Date(Date.now() + 365 * 86400 * 1000).toISOString(),
      notes: 'Seeded demo device. Use serial MG-DEMO-0001.',
    });
    addEvent(db, device, 'device.seeded', { serial: device.serial });
  }
  return device;
}
