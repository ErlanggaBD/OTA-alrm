import crypto from 'node:crypto';

export function sha256Hex(input) {
  return crypto.createHash('sha256').update(input).digest('hex');
}

export function hmacHex(secret, input) {
  return crypto.createHmac('sha256', secret).update(input).digest('hex');
}

export function randomHex(bytes = 16) {
  return crypto.randomBytes(bytes).toString('hex');
}

export function safeEqualHex(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const ba = Buffer.from(a, 'hex');
  const bb = Buffer.from(b, 'hex');
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}

export function encryptSecret(plain, masterKey) {
  const iv = crypto.randomBytes(12);
  const cipher = crypto.createCipheriv('aes-256-gcm', masterKey, iv);
  const ciphertext = Buffer.concat([cipher.update(plain, 'utf8'), cipher.final()]);
  const tag = cipher.getAuthTag();
  return `v1:${iv.toString('base64')}:${tag.toString('base64')}:${ciphertext.toString('base64')}`;
}

export function decryptSecret(payload, masterKey) {
  const [version, ivB64, tagB64, dataB64] = String(payload).split(':');
  if (version !== 'v1' || !ivB64 || !tagB64 || !dataB64) {
    throw new Error('invalid encrypted secret format');
  }
  const decipher = crypto.createDecipheriv(
    'aes-256-gcm',
    masterKey,
    Buffer.from(ivB64, 'base64'),
  );
  decipher.setAuthTag(Buffer.from(tagB64, 'base64'));
  return Buffer.concat([
    decipher.update(Buffer.from(dataB64, 'base64')),
    decipher.final(),
  ]).toString('utf8');
}

/**
 * Canonical device signature:
 *   METHOD \n PATH \n TIMESTAMP \n SHA256(body)
 */
export function deviceCanonical(method, requestPath, timestamp, body) {
  return `${method.toUpperCase()}\n${requestPath}\n${timestamp}\n${sha256Hex(body || '')}`;
}

export function signDeviceRequest(secret, method, requestPath, timestamp, body = '') {
  return hmacHex(secret, deviceCanonical(method, requestPath, timestamp, body));
}
