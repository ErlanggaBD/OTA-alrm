import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import request from 'supertest';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createApp } from '../src/app.js';
import { loadConfig } from '../src/config.js';
import { ensureAdmin, openDb } from '../src/db.js';
import { hmacHex, signDeviceRequest } from '../src/crypto.js';

const EMAIL = 'test.device@example.com';
const SERIAL = 'MG-TEST-0001';
const ADMIN_USER = 'admin';
const ADMIN_PASS = 'test-admin-pass';

let tmpDir;
let config;
let db;
let app;
let secret;

function signedHeaders(deviceSecret, method, requestPath, body = '') {
  const timestamp = String(Math.floor(Date.now() / 1000));
  const nonce = crypto.randomBytes(12).toString('hex');
  const signature = signDeviceRequest(deviceSecret, method, requestPath, timestamp, body);
  return {
    'X-Device-Email': EMAIL,
    'X-Timestamp': timestamp,
    'X-Nonce': nonce,
    'X-Signature': signature,
  };
}

function csrfFrom(html) {
  const match = html.match(/name="_csrf"\s+value="([^"]+)"/);
  if (!match) throw new Error('CSRF token not found in HTML');
  return match[1];
}

beforeAll(() => {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'medgas-test-'));
  config = loadConfig({
    dbPath: path.join(tmpDir, 'test.sqlite'),
    firmwareDir: path.join(tmpDir, 'firmware'),
    masterKey: 'test-master-key',
    sessionSecret: 'test-session-secret',
    isTest: true,
  });
  config.adminUser = ADMIN_USER;
  db = openDb(config);
  ensureAdmin(db, ADMIN_USER, ADMIN_PASS);
  app = createApp(config, db);
});

afterAll(() => {
  db?.close();
  if (tmpDir) fs.rmSync(tmpDir, { recursive: true, force: true });
});

describe('device API', () => {
  it('provisions a new device and returns a secret + config', async () => {
    const res = await request(app).post('/api/v1/provision').send({
      email: EMAIL,
      serial: SERIAL,
      hw_rev: 'esp32-devkit-4mb',
      fw_version: '1.0.0',
    });

    expect(res.status).toBe(200);
    expect(res.body.ok).toBe(true);
    expect(res.body.device_secret).toMatch(/^[0-9a-f]{64}$/);
    expect(res.body.config.active_sensors).toBe(4);
    expect(res.body.config.auto_ota).toBe(false);
    secret = res.body.device_secret;
  });

  it('serves a signed config and rejects nonce replay', async () => {
    const requestPath = `/api/v1/devices/${EMAIL}/config`;
    const headers = signedHeaders(secret, 'GET', requestPath);

    const res = await request(app).get(requestPath).set(headers);
    expect(res.status).toBe(200);
    expect(res.headers['x-signature']).toBe(hmacHex(secret, res.text));
    expect(res.body.active_sensors).toBe(4);
    expect(res.body.quota).toBeTruthy();

    const replay = await request(app).get(requestPath).set(headers);
    expect(replay.status).toBe(401);
  });

  it('accepts a signed heartbeat', async () => {
    const requestPath = `/api/v1/devices/${EMAIL}/heartbeat`;
    const payload = {
      fw_version: '1.0.0',
      active_sensors: 4,
      wifi_rssi: -55,
      uptime: 120,
      alarm_state: 'normal',
      online: true,
    };
    const body = JSON.stringify(payload);
    const res = await request(app)
      .post(requestPath)
      .set('Content-Type', 'application/json')
      .set(signedHeaders(secret, 'POST', requestPath, body))
      .send(body);

    expect(res.status).toBe(200);
    expect(res.headers['x-signature']).toBe(hmacHex(secret, res.text));
    expect(res.body.ok).toBe(true);
  });
});

describe('admin quota control', () => {
  it('updates active sensors, lock policy and auto_ota by email', async () => {
    const agent = request.agent(app);

    const loginPage = await agent.get('/admin/login');
    expect(loginPage.status).toBe(200);
    const login = await agent
      .post('/admin/login')
      .type('form')
      .send({ _csrf: csrfFrom(loginPage.text), username: ADMIN_USER, password: ADMIN_PASS });
    expect(login.status).toBe(302);

    const dashboard = await agent.get('/admin');
    expect(dashboard.status).toBe(200);
    const deviceId = dashboard.text.match(/\/admin\/devices\/(\d+)/)?.[1];
    expect(deviceId).toBeTruthy();

    const devicePage = await agent.get(`/admin/devices/${deviceId}`);
    expect(devicePage.status).toBe(200);

    const edit = await agent
      .post(`/admin/devices/${deviceId}`)
      .type('form')
      .send({
        _csrf: csrfFrom(devicePage.text),
        email: EMAIL,
        serial: SERIAL,
        hw_rev: 'esp32-devkit-4mb',
        active_sensors: '6',
        default_active_sensors: '4',
        lock_policy: 'grace',
        grace_days: '7',
        offline_action: 'keep_last',
        allow_local_override: '1',
        auto_ota: '1',
        plan: 'basic',
        fw_channel: 'stable',
        notes: 'test device',
      });
    expect(edit.status).toBe(302);

    const requestPath = `/api/v1/devices/${EMAIL}/config`;
    const res = await request(app).get(requestPath).set(signedHeaders(secret, 'GET', requestPath));
    expect(res.status).toBe(200);
    expect(res.body.active_sensors).toBe(6);
    expect(res.body.lock_policy).toBe('grace');
    expect(res.body.auto_ota).toBe(true);
  });
});
