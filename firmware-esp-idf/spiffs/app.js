/* MedGas Alarm local dashboard — vanilla JS, no build step. */
(() => {
  'use strict';

  const page = document.body.dataset.page || '';
  const state = {
    system: null,
    config: null,
    sensors: [],
    csrf: '',
    spark: {},
    poll: null,
    otaPoll: null,
    setupRecovery: false,
  };

  const $ = (id) => document.getElementById(id);
  const on = (id, event, fn) => {
    const el = $(id);
    if (el) el.addEventListener(event, fn);
  };

  function fmtDate(epoch) {
    if (!epoch) return '—';
    const d = new Date(epoch * 1000);
    if (Number.isNaN(d.getTime())) return '—';
    return d.toLocaleString('id-ID', {
      day: '2-digit',
      month: 'short',
      year: 'numeric',
      hour: '2-digit',
      minute: '2-digit',
    });
  }

  function fmtDuration(sec) {
    if (sec === null || sec === undefined) return '—';
    const s = Math.max(0, Math.floor(sec));
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    if (d) return `${d}h ${h}j`;
    if (h) return `${h}j ${m}m`;
    return `${m}m`;
  }

  function fmtValue(value, digits = 1) {
    if (value === null || value === undefined || Number.isNaN(Number(value))) return '—';
    const n = Number(value);
    return Number.isInteger(n) ? String(n) : n.toFixed(digits);
  }

  function escapeHtml(value) {
    return String(value ?? '')
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#39;');
  }

  function showToast(message, type = 'info') {
    const el = $('toast');
    if (!el) return;
    el.textContent = message;
    el.className = `toast ${type}`;
    clearTimeout(showToast._timer);
    showToast._timer = setTimeout(() => el.classList.add('hidden'), 4200);
  }

  async function api(path, options = {}) {
    const headers = new Headers(options.headers || {});
    if (options.body && typeof options.body === 'string' && !headers.has('Content-Type')) {
      headers.set('Content-Type', 'application/json');
    }
    if (state.csrf && options.method && options.method !== 'GET') {
      headers.set('X-CSRF-Token', state.csrf);
    }
    const res = await fetch(path, {
      credentials: 'same-origin',
      cache: 'no-store',
      ...options,
      headers,
    });
    if (res.status === 401 && !['login', 'setup'].includes(page)) {
      window.location.href = '/login';
      throw new Error('unauthorized');
    }
    if (res.status === 423) {
      window.location.href = '/locked';
      throw new Error('locked');
    }
    const ct = res.headers.get('content-type') || '';
    const data = ct.includes('application/json') ? await res.json() : await res.text();
    if (!res.ok) {
      const msg = typeof data === 'object' ? data.error || JSON.stringify(data) : data;
      throw new Error(msg || `HTTP ${res.status}`);
    }
    return data;
  }

  async function logout() {
    try {
      await api('/api/logout', { method: 'POST' });
    } catch {
      /* ignore */
    }
    window.location.href = '/login';
  }

  function bindLogout() {
    on('logout-btn', 'click', logout);
  }

  function sparkline(values) {
    if (!values || values.length < 2) return '';
    const w = 140;
    const h = 36;
    const min = Math.min(...values);
    const max = Math.max(...values);
    const span = max - min || 1;
    const points = values
      .map((v, i) => {
        const x = (i / (values.length - 1)) * w;
        const y = h - ((v - min) / span) * (h - 4) - 2;
        return `${x.toFixed(1)},${y.toFixed(1)}`;
      })
      .join(' ');
    return `<svg class="spark" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" aria-hidden="true"><polyline points="${points}"/></svg>`;
  }

  /* ------------------------------------------------------------------ */
  /* Dashboard                                                          */
  /* ------------------------------------------------------------------ */

  function renderSystem(sys) {
    state.system = sys;
    state.csrf = sys.csrf || state.csrf;

    if (sys.locked) {
      window.location.href = '/locked';
      return;
    }

    const email = sys.device?.email || '—';
    const emailEl = $('device-email');
    if (emailEl) emailEl.textContent = email;

    const cloudChip = $('cloud-chip');
    if (cloudChip) {
      const online = Boolean(sys.cloud?.online);
      cloudChip.textContent = online ? 'Cloud online' : 'Cloud offline';
      cloudChip.className = `chip ${online ? 'ok' : 'warn'}`;
    }
    const wifiChip = $('wifi-chip');
    if (wifiChip) {
      const rssi = sys.device?.rssi;
      wifiChip.textContent = rssi === null || rssi === undefined ? 'Wi‑Fi —' : `Wi‑Fi ${rssi} dBm`;
      wifiChip.className = `chip ${rssi > -70 ? 'ok' : rssi > -82 ? 'warn' : 'bad'}`;
    }

    const active = sys.sensors?.active ?? 0;
    const total = sys.sensors?.total ?? 8;
    if ($('stat-sensors')) $('stat-sensors').textContent = `${active} / ${total}`;
    if ($('stat-sensors-note')) $('stat-sensors-note').textContent = 'kuota paket aktif';
    if ($('sensor-count')) $('sensor-count').textContent = `${active} dari ${total} sensor aktif`;

    const alarm = sys.alarm || {};
    const alarmState = alarm.state || 'normal';
    if ($('stat-alarm')) $('stat-alarm').textContent = alarmState.toUpperCase();
    if ($('stat-alarm-note')) {
      $('stat-alarm-note').textContent = alarm.latched
        ? 'latched — perlu acknowledge'
        : alarm.acknowledged
          ? 'di-acknowledge'
          : 'tidak ada alarm aktif';
    }

    if ($('stat-sync')) {
      $('stat-sync').textContent = sys.cloud?.last_sync ? fmtDate(sys.cloud.last_sync) : 'Belum sync';
    }
    if ($('stat-sync-note')) {
      $('stat-sync-note').textContent = sys.cloud?.online
        ? `plan ${sys.cloud?.plan || '—'}`
        : 'memakai cache terakhir';
    }
    if ($('stat-fw')) $('stat-fw').textContent = sys.device?.fw || '—';
    if ($('stat-fw-note')) $('stat-fw-note').textContent = sys.ota?.available ? 'update tersedia' : 'up to date';

    renderAlarmBanner(sys);
  }

  function renderAlarmBanner(sys) {
    const banner = $('alarm-banner');
    if (!banner) return;
    const alarm = sys.alarm || {};
    const stateName = alarm.state || 'normal';
    const triggered = Array.isArray(alarm.triggered) ? alarm.triggered : [];
    const labels = triggered.map((t) => t.label || `Sensor ${Number(t.idx) + 1}`);
    const ackBtn = $('ack-btn');

    banner.className = `banner ${stateName}`;
    if (stateName === 'alarm' || stateName === 'fault') {
      $('alarm-icon').textContent = '!';
      $('alarm-title').textContent = stateName === 'fault' ? 'Sensor fault' : 'ALARM AKTIF';
      $('alarm-detail').textContent = labels.length
        ? `${labels.join(', ')} di luar batas aman.`
        : 'Kondisi alarm terdeteksi.';
      if (ackBtn) ackBtn.classList.toggle('hidden', Boolean(alarm.acknowledged));
    } else if (stateName === 'warning') {
      $('alarm-icon').textContent = '△';
      $('alarm-title').textContent = 'Peringatan';
      $('alarm-detail').textContent = labels.length
        ? `${labels.join(', ')} mendekati batas.`
        : 'Beberapa sensor mendekati batas.';
      if (ackBtn) ackBtn.classList.add('hidden');
    } else {
      $('alarm-icon').textContent = '✓';
      $('alarm-title').textContent = 'Sistem normal';
      $('alarm-detail').textContent = 'Semua sensor aktif dalam batas aman.';
      if (ackBtn) ackBtn.classList.add('hidden');
    }
  }

  function sensorCard(sensor) {
    const idx = Number(sensor.idx);
    const values = state.spark[idx] || [];
    const value = sensor.fault ? 'FAULT' : fmtValue(sensor.value);
    const unit = sensor.fault ? '' : sensor.unit || '';
    const stateName = sensor.fault ? 'fault' : sensor.state || 'normal';
    const secondary = sensor.secondary !== null && sensor.secondary !== undefined
      ? `${fmtValue(sensor.secondary)}${sensor.secondary_unit ? ` ${sensor.secondary_unit}` : ''}`
      : '';
    return `
      <article class="sensor-card ${stateName}" data-sensor-index="${idx}">
        <div class="sensor-top">
          <span class="sensor-label">${escapeHtml(sensor.label || `Sensor ${idx + 1}`)}</span>
          <span class="state-badge ${stateName}">${stateName}</span>
        </div>
        <div class="sensor-value">${escapeHtml(value)}<span class="sensor-unit">${escapeHtml(unit)}</span></div>
        <div class="sensor-secondary">${secondary ? `Kelembapan ${escapeHtml(secondary)}` : '&nbsp;'}</div>
        ${sparkline(values)}
        <div class="sensor-foot">
          <span>${sensor.valid ? 'Terbaca' : 'Tidak valid'}</span>
          <span>${sensor.last_read ? `${fmtDuration(Math.max(0, (Date.now() / 1000) - sensor.last_read))} lalu` : '—'}</span>
        </div>
      </article>`;
  }

  function renderSensors(data) {
    const grid = $('sensor-grid');
    if (!grid) return;
    const sensors = Array.isArray(data.sensors) ? data.sensors : [];
    state.sensors = sensors;
    for (const sensor of sensors) {
      if (sensor.valid && !sensor.fault && Number.isFinite(Number(sensor.value))) {
        const idx = Number(sensor.idx);
        const arr = state.spark[idx] || (state.spark[idx] = []);
        arr.push(Number(sensor.value));
        if (arr.length > 40) arr.shift();
      }
    }
    if (!sensors.length) {
      grid.innerHTML = '<p class="muted empty-state">Tidak ada sensor aktif pada paket ini.</p>';
      return;
    }
    grid.innerHTML = sensors.map(sensorCard).join('');
  }

  async function refreshDashboard() {
    try {
      const [sys, sensors] = await Promise.all([api('/api/system'), api('/api/sensors')]);
      renderSystem(sys);
      renderSensors(sensors);
    } catch (err) {
      if (err.message !== 'unauthorized' && err.message !== 'locked') {
        showToast(err.message, 'error');
      }
    }
  }

  async function loadLogs() {
    const box = $('logs');
    if (!box) return;
    try {
      const data = await api('/api/logs?limit=40');
      const logs = Array.isArray(data.logs) ? data.logs : [];
      box.innerHTML = logs.length
        ? logs
            .map(
              (l) =>
                `<div class="log-line"><span class="log-time">${escapeHtml(l.time || '')}</span><span class="log-level ${escapeHtml(l.level || 'info')}">${escapeHtml(l.level || 'info')}</span><span>${escapeHtml(l.message || '')}</span></div>`,
            )
            .join('')
        : '<p class="muted">Belum ada log.</p>';
    } catch (err) {
      box.innerHTML = `<p class="form-error">${escapeHtml(err.message)}</p>`;
    }
  }

  async function initDashboard() {
    bindLogout();
    on('ack-btn', 'click', async () => {
      try {
        await api('/api/alarm/ack', { method: 'POST', body: '{}' });
        showToast('Alarm di-acknowledge', 'ok');
        await refreshDashboard();
      } catch (err) {
        showToast(err.message, 'error');
      }
    });
    on('refresh-logs', 'click', loadLogs);
    await refreshDashboard();
    await loadLogs();
    state.poll = setInterval(refreshDashboard, 2000);
    setInterval(loadLogs, 10000);
  }

  /* ------------------------------------------------------------------ */
  /* Settings                                                           */
  /* ------------------------------------------------------------------ */

  function renderDeviceInfo(sys) {
    const box = $('device-info');
    if (!box) return;
    const d = sys.device || {};
    const rows = [
      ['Email ID', d.email || '—'],
      ['Serial', d.serial || '—'],
      ['Hardware', d.hw || '—'],
      ['Firmware', d.fw || '—'],
      ['IP lokal', d.ip || '—'],
      ['Uptime', fmtDuration(d.uptime)],
      ['RSSI', d.rssi !== null && d.rssi !== undefined ? `${d.rssi} dBm` : '—'],
      ['Mode', sys.ap_mode ? 'Access Point' : 'Station'],
    ];
    box.innerHTML = rows
      .map(
        ([k, v]) =>
          `<div class="kv"><span>${escapeHtml(k)}</span><strong>${escapeHtml(v)}</strong></div>`,
      )
      .join('');
    const cloud = $('settings-cloud');
    if (cloud) {
      cloud.textContent = sys.cloud?.online ? 'Cloud online' : 'Cloud offline';
      cloud.className = `chip ${sys.cloud?.online ? 'ok' : 'warn'}`;
    }
  }

  function renderThresholds(config) {
    const body = $('threshold-body');
    if (!body) return;
    const active = Number(config.active_sensors || 0);
    if ($('threshold-count')) $('threshold-count').textContent = `${active} sensor aktif`;
    const thresholds = Array.isArray(config.thresholds) ? config.thresholds : [];
    const labels = Array.isArray(config.sensor_labels) ? config.sensor_labels : [];
    body.innerHTML = '';
    for (let i = 0; i < active; i += 1) {
      const t = thresholds[i] || {};
      const tr = document.createElement('tr');
      tr.dataset.idx = String(i);
      tr.innerHTML = `
        <td><strong>${escapeHtml(labels[i] || `Sensor ${i + 1}`)}</strong><br><small class="muted">#${i + 1}</small></td>
        <td><input data-field="warn_low" type="number" step="0.1" value="${escapeHtml(t.warn_low ?? 5)}"></td>
        <td><input data-field="alarm_low" type="number" step="0.1" value="${escapeHtml(t.alarm_low ?? 2)}"></td>
        <td><input data-field="warn_high" type="number" step="0.1" value="${escapeHtml(t.warn_high ?? 35)}"></td>
        <td><input data-field="alarm_high" type="number" step="0.1" value="${escapeHtml(t.alarm_high ?? 40)}"></td>
        <td><input data-field="hysteresis" type="number" step="0.1" value="${escapeHtml(t.hysteresis ?? 0.5)}"></td>
        <td><input data-field="unit" value="${escapeHtml(t.unit || 'C')}" maxlength="7"></td>`;
      body.appendChild(tr);
    }
  }

  function renderSettings(sys, config) {
    state.system = sys;
    state.config = config;
    state.csrf = sys.csrf || state.csrf;
    if (sys.locked) {
      window.location.href = '/locked';
      return;
    }
    renderDeviceInfo(sys);
    if ($('active-sensors')) $('active-sensors').value = `${config.active_sensors} / 8`;
    if ($('plan')) $('plan').value = config.quota?.plan || '—';
    if ($('quota-expires')) {
      $('quota-expires').value = config.quota?.expires_at
        ? String(config.quota.expires_at).slice(0, 10)
        : 'tanpa batas';
    }
    if ($('lock-policy')) {
      $('lock-policy').value = config.lock_policy || 'provision_only';
      $('lock-policy').disabled = !config.allow_local_override;
    }
    if ($('offline-action')) $('offline-action').value = config.offline_action || 'keep_last';
    if ($('grace-days')) $('grace-days').value = config.grace_days ?? 7;
    renderThresholds(config);

    const ota = sys.ota || {};
    if ($('ota-status')) {
      $('ota-status').textContent = ota.status || 'idle';
      $('ota-status').className = `chip ${ota.status === 'error' ? 'bad' : ota.status === 'idle' ? '' : 'warn'}`;
    }
    const cloudBtn = $('ota-cloud');
    if (cloudBtn) {
      cloudBtn.classList.toggle('hidden', !ota.available);
      if (ota.available) cloudBtn.textContent = `Install ${ota.version || 'update'} dari cloud`;
    }
    if ($('ota-note')) {
      $('ota-note').textContent = ota.available
        ? `Firmware ${ota.version} tersedia (${ota.size ? `${(ota.size / 1024).toFixed(0)} KB` : 'ukuran tidak diketahui'}).`
        : 'Tidak ada update cloud untuk channel ini.';
    }
  }

  async function refreshSettings() {
    try {
      const [sys, config] = await Promise.all([api('/api/system'), api('/api/config')]);
      renderSettings(sys, config);
    } catch (err) {
      if (err.message !== 'unauthorized' && err.message !== 'locked') {
        showToast(err.message, 'error');
      }
    }
  }

  async function savePolicy() {
    const policy = $('lock-policy')?.value;
    if (!policy) return;
    try {
      await api('/api/config', {
        method: 'POST',
        body: JSON.stringify({ lock_policy: policy }),
      });
      showToast('Kebijakan disimpan', 'ok');
      await refreshSettings();
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function saveThresholds() {
    if (!state.config) return;
    const thresholds = JSON.parse(JSON.stringify(state.config.thresholds || []));
    while (thresholds.length < 8) {
      thresholds.push({ idx: thresholds.length, warn_low: 5, alarm_low: 2, warn_high: 35, alarm_high: 40, hysteresis: 0.5, unit: 'C' });
    }
    const rows = document.querySelectorAll('#threshold-body tr');
    for (const row of rows) {
      const idx = Number(row.dataset.idx);
      const get = (field) => row.querySelector(`[data-field="${field}"]`)?.value;
      thresholds[idx] = {
        idx,
        warn_low: Number(get('warn_low')),
        alarm_low: Number(get('alarm_low')),
        warn_high: Number(get('warn_high')),
        alarm_high: Number(get('alarm_high')),
        hysteresis: Number(get('hysteresis')),
        unit: String(get('unit') || 'C').slice(0, 7),
      };
    }
    try {
      await api('/api/config', {
        method: 'POST',
        body: JSON.stringify({ thresholds }),
      });
      showToast('Threshold disimpan', 'ok');
      await refreshSettings();
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function syncNow() {
    try {
      const res = await api('/api/cloud/sync', { method: 'POST', body: '{}' });
      showToast(res.message || 'Sync selesai', 'ok');
      await refreshSettings();
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function sha256Hex(file) {
    if (!window.crypto?.subtle) return '';
    const buffer = await file.arrayBuffer();
    const digest = await crypto.subtle.digest('SHA-256', buffer);
    return Array.from(new Uint8Array(digest))
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');
  }

  function setOtaProgress(percent, text) {
    const wrap = $('ota-progress-wrap');
    const bar = $('ota-progress');
    const label = $('ota-progress-text');
    if (wrap) wrap.classList.remove('hidden');
    if (bar) bar.style.width = `${Math.max(0, Math.min(100, percent))}%`;
    if (label) label.textContent = text || `${Math.round(percent)}%`;
  }

  async function uploadOta() {
    const input = $('ota-file');
    const file = input?.files?.[0];
    if (!file) {
      showToast('Pilih file firmware .bin dulu', 'error');
      return;
    }
    const btn = $('ota-upload');
    if (btn) btn.disabled = true;
    try {
      setOtaProgress(0, 'Menghitung SHA-256…');
      const hash = await sha256Hex(file);
      setOtaProgress(1, 'Mengunggah…');
      await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/ota');
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        if (state.csrf) xhr.setRequestHeader('X-CSRF-Token', state.csrf);
        if (hash) xhr.setRequestHeader('X-Firmware-SHA256', hash);
        xhr.upload.onprogress = (event) => {
          if (!event.lengthComputable) return;
          const pct = (event.loaded / event.total) * 100;
          setOtaProgress(pct, `${pct.toFixed(0)}% diunggah`);
        };
        xhr.onload = () => {
          let data = {};
          try {
            data = JSON.parse(xhr.responseText || '{}');
          } catch {
            data = {};
          }
          if (xhr.status >= 200 && xhr.status < 300) {
            setOtaProgress(100, 'Selesai, alat akan reboot…');
            showToast('Firmware terpasang. Menunggu reboot…', 'ok');
            setTimeout(() => window.location.reload(), 12000);
            resolve();
          } else {
            reject(new Error(data.error || `Upload gagal (HTTP ${xhr.status})`));
          }
        };
        xhr.onerror = () => reject(new Error('Koneksi terputus saat upload'));
        xhr.send(file);
      });
    } catch (err) {
      showToast(err.message, 'error');
      setOtaProgress(0, 'Gagal');
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  async function cloudOta() {
    try {
      const res = await api('/api/ota/cloud', { method: 'POST', body: '{}' });
      showToast(res.message || 'Update cloud dimulai', 'ok');
      await refreshSettings();
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function factoryReset() {
    const confirm = $('reset-confirm')?.value || '';
    if (confirm !== 'RESET') {
      showToast('Ketik RESET untuk konfirmasi', 'error');
      return;
    }
    if (!window.confirm('Factory reset menghapus semua konfigurasi. Lanjutkan?')) return;
    try {
      await api('/api/factory-reset', {
        method: 'POST',
        body: JSON.stringify({ confirm: 'RESET' }),
      });
      showToast('Factory reset. Alat reboot ke mode registrasi…', 'ok');
      setTimeout(() => window.location.reload(), 12000);
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function reboot() {
    if (!window.confirm('Reboot alat sekarang?')) return;
    try {
      await api('/api/reboot', { method: 'POST', body: '{}' });
      showToast('Rebooting…', 'ok');
      setTimeout(() => window.location.reload(), 10000);
    } catch (err) {
      showToast(err.message, 'error');
    }
  }

  async function initSettings() {
    bindLogout();
    on('save-policy', 'click', savePolicy);
    on('save-thresholds', 'click', saveThresholds);
    on('sync-now', 'click', syncNow);
    on('ota-upload', 'click', uploadOta);
    on('ota-cloud', 'click', cloudOta);
    on('factory-reset', 'click', factoryReset);
    on('reboot', 'click', reboot);
    on('refresh-logs', 'click', loadLogs);
    await refreshSettings();
    await loadLogs();
    state.poll = setInterval(refreshSettings, 5000);
  }

  /* ------------------------------------------------------------------ */
  /* Setup / captive portal                                             */
  /* ------------------------------------------------------------------ */

  async function scanWifi() {
    const btn = $('scan-btn');
    if (btn) btn.disabled = true;
    try {
      const data = await api('/api/scan');
      const select = $('setup-ssid-select');
      if (select) {
        select.innerHTML = '<option value="">— pilih jaringan —</option>';
        for (const net of data.networks || []) {
          const option = document.createElement('option');
          option.value = net.ssid;
          option.textContent = `${net.ssid} (${net.rssi} dBm)${net.secure ? ' 🔒' : ''}`;
          select.appendChild(option);
        }
      }
      showToast(`${(data.networks || []).length} jaringan ditemukan`, 'ok');
    } catch (err) {
      showToast(err.message, 'error');
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  function setSetupStatus(title, detail, type = 'info') {
    const banner = $('setup-status');
    if (!banner) return;
    banner.className = `banner ${type}`;
    const titleEl = banner.querySelector('strong');
    const detailEl = banner.querySelector('span');
    if (titleEl) titleEl.textContent = title;
    if (detailEl) detailEl.textContent = detail;
  }

  async function submitSetup(event) {
    event.preventDefault();
    const email = $('setup-email')?.value.trim();
    const ssid = $('setup-ssid')?.value.trim();
    const password = $('setup-wifi-pass')?.value || '';
    const adminPassword = $('setup-admin-pass')?.value || '';
    const apiBase = $('setup-api-base')?.value.trim() || '';
    const errorEl = $('setup-error');
    const btn = $('setup-submit');

    if (!email || !ssid || adminPassword.length < 8) {
      if (errorEl) {
        errorEl.textContent = 'Email, SSID, dan password admin (min. 8 karakter) wajib diisi.';
        errorEl.classList.remove('hidden');
      }
      return;
    }
    if (errorEl) errorEl.classList.add('hidden');
    if (btn) btn.disabled = true;
    setSetupStatus('Menghubungkan…', 'Alat mencoba terhubung ke Wi‑Fi dan server vendor.', 'info');
    setOtaProgress(5, 'Menghubungkan Wi‑Fi…');
    const progress = $('setup-progress');
    if (progress) progress.classList.remove('hidden');
    try {
      const res = await api('/api/provision', {
        method: 'POST',
        body: JSON.stringify({
          email,
          ssid,
          password,
          admin_password: adminPassword,
          api_base: apiBase,
        }),
      });
      setOtaProgress(100, 'Berhasil');
      setSetupStatus('Registrasi berhasil', 'Konfigurasi disimpan. Alat akan reboot ke dashboard.', 'ok');
      showToast(res.message || 'Registrasi berhasil. Menunggu reboot…', 'ok');
      setTimeout(() => window.location.reload(), 12000);
    } catch (err) {
      setOtaProgress(0, 'Gagal');
      setSetupStatus('Registrasi gagal', err.message, 'error');
      if (errorEl) {
        errorEl.textContent = err.message;
        errorEl.classList.remove('hidden');
      }
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  async function initSetup() {
    on('scan-btn', 'click', scanWifi);
    on('setup-ssid-select', 'change', (event) => {
      const value = event.target.value;
      if (value) {
        const ssid = $('setup-ssid');
        if (ssid) ssid.value = value;
      }
    });
    on('setup-form', 'submit', submitSetup);
    try {
      const sys = await api('/api/system');
      state.system = sys;
      state.csrf = sys.csrf || '';
      if (sys.provisioned) {
        state.setupRecovery = true;
        if ($('setup-mode')) $('setup-mode').textContent = 'Mode pemulihan';
        if ($('setup-title')) $('setup-title').textContent = 'Perbaiki koneksi';
        if ($('setup-intro')) {
          $('setup-intro').textContent =
            'Alat sudah terdaftar. Masukkan password admin lama untuk mengganti Wi‑Fi atau memperbaiki koneksi.';
        }
        if ($('admin-pass-hint')) {
          $('admin-pass-hint').textContent = 'Gunakan password admin yang sudah ada.';
        }
        if ($('setup-email')) {
          $('setup-email').value = sys.device?.email || '';
          $('setup-email').readOnly = true;
        }
        if ($('setup-ssid')) $('setup-ssid').value = sys.device?.ssid || '';
      }
    } catch {
      /* AP mode may not expose system yet; the form still works. */
    }
    scanWifi();
  }

  /* ------------------------------------------------------------------ */
  /* Login / locked                                                     */
  /* ------------------------------------------------------------------ */

  async function initLogin() {
    on('login-form', 'submit', async (event) => {
      event.preventDefault();
      const password = $('login-password')?.value || '';
      const errorEl = $('login-error');
      try {
        await api('/api/login', {
          method: 'POST',
          body: JSON.stringify({ password }),
        });
        window.location.href = '/';
      } catch (err) {
        if (errorEl) {
          errorEl.textContent = err.message;
          errorEl.classList.remove('hidden');
        }
      }
    });
  }

  async function initLocked() {
    on('retry-btn', 'click', async () => {
      try {
        const sys = await api('/api/system');
        if (!sys.locked) {
          window.location.href = '/';
          return;
        }
        const reason = $('lock-reason');
        if (reason && sys.lock_reason) reason.textContent = sys.lock_reason;
        showToast('Masih terkunci. Periksa koneksi internet.', 'error');
      } catch (err) {
        showToast(err.message, 'error');
      }
    });
    try {
      const sys = await api('/api/system');
      if (!sys.locked) {
        window.location.href = '/';
        return;
      }
      const reason = $('lock-reason');
      if (reason && sys.lock_reason) reason.textContent = sys.lock_reason;
    } catch {
      /* keep the generic message */
    }
  }

  /* ------------------------------------------------------------------ */
  /* Boot                                                               */
  /* ------------------------------------------------------------------ */

  document.addEventListener('DOMContentLoaded', () => {
    if (page === 'dashboard') initDashboard();
    else if (page === 'settings') initSettings();
    else if (page === 'setup') initSetup();
    else if (page === 'login') initLogin();
    else if (page === 'locked') initLocked();
  });
})();
