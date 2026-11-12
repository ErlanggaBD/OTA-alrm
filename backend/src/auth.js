import { decryptSecret, hmacHex, randomHex, safeEqualHex, signDeviceRequest } from './crypto.js';
import { getDeviceByEmail, useNonce } from './db.js';

export function signedJson(res, payload, secret) {
  const body = JSON.stringify(payload);
  res.type('application/json');
  // Response signature covers only the exact JSON body.
  if (secret) res.set('X-Signature', hmacHex(secret, body));
  return res.send(body);
}

/**
 * Verify device HMAC. The canonical path is taken from originalUrl without
 * the query string, so firmware must sign exactly that path.
 */
export function deviceAuth(db, config) {
  return (req, res, next) => {
    const email = String(req.headers['x-device-email'] || req.params.email || '');
    const timestamp = String(req.headers['x-timestamp'] || '');
    const nonce = String(req.headers['x-nonce'] || '');
    const signature = String(req.headers['x-signature'] || '');
    if (!email || !timestamp || !nonce || !signature) {
      return res.status(401).json({ error: 'missing device authentication headers' });
    }

    const device = getDeviceByEmail(db, email);
    if (!device) return res.status(401).json({ error: 'unknown device' });

    const now = Math.floor(Date.now() / 1000);
    const ts = Number.parseInt(timestamp, 10);
    if (!Number.isFinite(ts) || Math.abs(now - ts) > config.hmacWindowSec) {
      return res.status(401).json({ error: 'stale timestamp' });
    }
    if (nonce.length < 8 || nonce.length > 128) {
      return res.status(401).json({ error: 'invalid nonce' });
    }

    let secret;
    try {
      secret = decryptSecret(device.secret_enc, config.masterKey);
    } catch {
      return res.status(500).json({ error: 'device secret cannot be decrypted' });
    }

    const body = req.rawBody ? req.rawBody.toString('utf8') : '';
    const canonicalPath = req.originalUrl.split('?')[0];
    const expected = signDeviceRequest(secret, req.method, canonicalPath, timestamp, body);
    if (!safeEqualHex(signature, expected)) {
      return res.status(401).json({ error: 'bad signature' });
    }
    if (!useNonce(db, nonce, device.id, now, config.hmacWindowSec)) {
      return res.status(401).json({ error: 'nonce already used' });
    }

    req.device = device;
    req.deviceSecret = secret;
    next();
  };
}

export function csrfToken(req, res, next) {
  if (!req.session.csrf) req.session.csrf = randomHex(24);
  res.locals.csrf = req.session.csrf;
  next();
}

export function verifyCsrf(req, res, next) {
  const token = req.body?._csrf || req.headers['x-csrf-token'] || '';
  if (!req.session?.csrf || token !== req.session.csrf) {
    return res.status(403).send('CSRF token invalid');
  }
  next();
}

export function requireAdmin(req, res, next) {
  if (!req.session?.admin) return res.redirect('/admin/login');
  res.locals.admin = req.session.admin;
  next();
}
