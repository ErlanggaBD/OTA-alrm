import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import dotenv from 'dotenv';

dotenv.config();

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(__dirname, '..');

function bool(value, fallback = false) {
  if (value === undefined || value === null || value === '') return fallback;
  return ['1', 'true', 'yes', 'on'].includes(String(value).toLowerCase());
}

function int(value, fallback) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? n : fallback;
}

export function loadConfig(overrides = {}) {
  const masterKeyMaterial =
    overrides.masterKey || process.env.MASTER_KEY || 'dev-master-key-change-me';
  // Always derive a stable 32-byte key. Production should supply 64 hex chars.
  const masterKey = /^[0-9a-f]{64}$/i.test(masterKeyMaterial)
    ? Buffer.from(masterKeyMaterial, 'hex')
    : crypto.createHash('sha256').update(masterKeyMaterial).digest();

  const dbPath = path.resolve(
    rootDir,
    overrides.dbPath || process.env.DB_PATH || './data/medgas.sqlite',
  );
  const firmwareDir = path.resolve(
    rootDir,
    overrides.firmwareDir || process.env.FIRMWARE_DIR || './data/firmware',
  );

  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  fs.mkdirSync(firmwareDir, { recursive: true });

  return {
    rootDir,
    port: int(process.env.PORT, 3000),
    dbPath,
    firmwareDir,
    masterKey,
    sessionSecret:
      overrides.sessionSecret || process.env.SESSION_SECRET || 'dev-session-secret',
    adminUser: process.env.ADMIN_USER || 'admin',
    adminPass: process.env.ADMIN_PASS || 'admin123',
    publicBaseUrl:
      (process.env.PUBLIC_BASE_URL || `http://localhost:${int(process.env.PORT, 3000)}`).replace(/\/$/, ''),
    allowSelfRegister: bool(process.env.ALLOW_SELF_REGISTER, true),
    provisionKey: process.env.PROVISION_KEY || '',
    hmacWindowSec: int(process.env.HMAC_WINDOW_SEC, 300),
    configTtlSec: int(process.env.CONFIG_TTL_SEC, 86400),
    isTest: overrides.isTest || false,
  };
}
