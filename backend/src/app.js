import path from 'node:path';
import express from 'express';
import helmet from 'helmet';
import session from 'express-session';
import { adminRoutes } from './routes/admin.js';
import { deviceRoutes } from './routes/device.js';

export function createApp(config, db) {
  const app = express();
  app.set('trust proxy', 1);
  app.set('view engine', 'ejs');
  app.set('views', path.join(config.rootDir, 'src/admin/views'));

  app.use(
    helmet({
      contentSecurityPolicy: {
        directives: {
          defaultSrc: ["'self'"],
          styleSrc: ["'self'", "'unsafe-inline'"],
          scriptSrc: ["'self'"],
          imgSrc: ["'self'", 'data:'],
          baseUri: ["'self'"],
          formAction: ["'self'"],
        },
      },
      crossOriginEmbedderPolicy: false,
    }),
  );

  app.use(
    express.json({
      limit: '64kb',
      verify: (req, res, buf) => {
        req.rawBody = buf;
      },
    }),
  );
  app.use(express.urlencoded({ extended: false, limit: '64kb' }));

  app.use(
    session({
      name: 'mg_admin',
      secret: config.sessionSecret,
      resave: false,
      saveUninitialized: false,
      cookie: {
        httpOnly: true,
        sameSite: 'lax',
        secure: process.env.NODE_ENV === 'production',
        maxAge: 8 * 60 * 60 * 1000,
      },
    }),
  );

  app.use('/admin-assets', express.static(path.join(config.rootDir, 'public')));
  app.use(
    '/firmware',
    express.static(config.firmwareDir, {
      setHeaders: (res) => {
        res.set('Cache-Control', 'public, max-age=3600');
      },
    }),
  );

  app.use('/api/v1', deviceRoutes(db, config));
  app.use('/admin', adminRoutes(db, config));

  app.get('/', (req, res) => res.redirect('/admin'));

  app.use((req, res) => {
    res.status(404).json({ error: 'not found' });
  });

  // eslint-disable-next-line no-unused-vars
  app.use((err, req, res, next) => {
    console.error(err);
    const status = err.status || 500;
    if (req.path.startsWith('/api/')) {
      return res.status(status).json({ error: err.message || 'internal error' });
    }
    return res.status(status).send(err.message || 'internal error');
  });

  return app;
}
