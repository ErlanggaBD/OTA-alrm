import { createApp } from './app.js';
import { loadConfig } from './config.js';
import { ensureAdmin, openDb } from './db.js';

const config = loadConfig();
const db = openDb(config);
ensureAdmin(db, config.adminUser, config.adminPass);

const app = createApp(config, db);
const server = app.listen(config.port, '0.0.0.0', () => {
  console.log(`[medgas] backend listening on http://0.0.0.0:${config.port}`);
  console.log(`[medgas] admin UI: ${config.publicBaseUrl}/admin`);
  console.log(`[medgas] database: ${config.dbPath}`);
  console.log(`[medgas] firmware: ${config.firmwareDir}`);
  if (config.allowSelfRegister) {
    console.log('[medgas] ALLOW_SELF_REGISTER=true (demo mode)');
  }
});

function shutdown(signal) {
  console.log(`[medgas] ${signal} received, shutting down`);
  server.close(() => {
    db.close();
    process.exit(0);
  });
}

process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
