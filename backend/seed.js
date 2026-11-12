import { loadConfig } from './src/config.js';
import { ensureAdmin, openDb, seedDemo } from './src/db.js';

const config = loadConfig();
const db = openDb(config);
ensureAdmin(db, config.adminUser, config.adminPass);
const device = seedDemo(db, config);

console.log('Seed selesai.');
console.log(`Admin     : ${config.adminUser} / ${config.adminPass}`);
console.log(`Demo email: ${device.email}`);
console.log(`Demo serial: ${device.serial}`);
console.log('Device secret dibuat ulang saat provisioning, jadi tidak perlu dicatat.');
db.close();
