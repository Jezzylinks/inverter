(() => {
  'use strict';

  /* ------------------------------------------------------------------ *
   *  VONIX inverter dashboard
   *  - REST:  /api/v1/{status,wifi,inverter,battery,solar,load,ota,services,faults,system}
   *  - Live:  WebSocket /ws  (authenticate -> subscribe -> status | device | ota)
   *  Values the hardware does not measure are shown as "Not measured",
   *  never as a made-up number.
   * ------------------------------------------------------------------ */

  const $ = (id) => document.getElementById(id);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const RANGES = { '15m': 15 * 60e3, '1h': 60 * 60e3, '6h': 6 * 60 * 60e3, session: Infinity };
  const RANGE_LABEL = { '15m': 'last 15 minutes', '1h': 'last hour', '6h': 'last 6 hours', session: 'this session' };
  /* The firmware does not define named operating modes; 0 is the default (normal) mode. */
  const MODE_NAMES = { 0: 'Normal' };
  const SAMPLE_GAP_MS = 4000;
  const SAMPLE_CAP = 6000;

  const store = {
    get(key, fallback) { try { const v = window.localStorage.getItem(key); return v === null ? fallback : v; } catch (_) { return fallback; } },
    set(key, value) { try { window.localStorage.setItem(key, value); } catch (_) { /* storage unavailable */ } }
  };
  const session = {
    get(key) { try { return window.sessionStorage.getItem(key) || ''; } catch (_) { return ''; } },
    set(key, value) { try { value ? window.sessionStorage.setItem(key, value) : window.sessionStorage.removeItem(key); } catch (_) { /* ignore */ } }
  };

  const state = {
    pin: session.get('inverterPin'),
    socket: null, socketRetry: 0, socketTimer: null, socketOk: false,
    apiOk: false, authNeeded: false,
    status: null, wifi: null, inverter: {}, battery: {}, solar: {}, load: {}, system: {}, services: null, ota: null,
    faults: { events: [], diagnostics: null, available: false },
    uptimeAt: 0, uptimeBase: 0,
    lastTelemetryAt: 0,
    samples: [],
    range: store.get('vonix.range', '15m'),
    clock24: store.get('vonix.clock24', '1') === '1',
    motion: store.get('vonix.motion', '1') === '1',
    liveNotifs: [],
    seen: { wifi: null, inv: null, otaVersion: '', batLow: false },
    activity: [],
    selectedSsid: ''
  };

  /* ------------------------------ helpers ------------------------------ */
  const keyCache = new Map();
  function nodes(key) {
    if (!keyCache.has(key)) keyCache.set(key, $$(`[data-k="${key}"]`));
    return keyCache.get(key);
  }
  function put(key, text) { const t = text == null || text === '' ? '—' : String(text); nodes(key).forEach((n) => { if (n.textContent !== t) n.textContent = t; }); }
  function show(key, visible) { nodes(key).forEach((n) => { n.hidden = !visible; }); }

  const isNum = (v) => v !== null && v !== undefined && v !== '' && Number.isFinite(Number(v));
  const num = (v, digits = 1) => (isNum(v) ? Number(v).toFixed(digits).replace(/\.0+$/, '') : null);
  const fmt = (v, unit = '', digits = 1) => { const n = num(v, digits); return n === null ? '—' : `${n}${unit}`; };

  function uptimeText(sec) {
    if (!isNum(sec)) return '—';
    sec = Math.max(0, Math.floor(Number(sec)));
    const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60);
    return d > 0 ? `${d}d ${h}h ${m}m` : h > 0 ? `${h}h ${m}m` : `${m}m ${sec % 60}s`;
  }
  function ago(ms) {
    const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
    if (s < 45) return 'just now';
    const m = Math.round(s / 60);
    if (m < 60) return `${m} min ago`;
    const h = Math.round(m / 60);
    if (h < 24) return `${h} hour${h === 1 ? '' : 's'} ago`;
    const d = Math.round(h / 24);
    return `${d} day${d === 1 ? '' : 's'} ago`;
  }
  function clockText(d) {
    const date = d.toLocaleDateString('en-US', { month: 'short', day: 'numeric', year: 'numeric' });
    const time = d.toLocaleTimeString('en-GB', { hour12: !state.clock24, hour: '2-digit', minute: '2-digit', second: '2-digit' });
    return `${date}   ${time}`;
  }
  function hhmm(ms) { return new Date(ms).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', hour12: false }); }

  function toast(message, kind = '') {
    const el = document.createElement('div');
    el.className = `toast ${kind}`;
    el.textContent = message;
    $('toasts').appendChild(el);
    window.setTimeout(() => el.remove(), 4200);
  }
  function logActivity(message, kind = '') {
    state.activity.unshift({ t: Date.now(), message, kind });
    state.activity.length = Math.min(state.activity.length, 60);
    renderActivity();
  }
  function setBusy(btn, busy) { if (btn) { btn.classList.toggle('loading', busy); btn.disabled = busy; } }
  function sigLevel(rssi) { if (!isNum(rssi) || Number(rssi) >= 0) return 0; const r = Number(rssi); return r >= -55 ? 4 : r >= -67 ? 3 : r >= -75 ? 2 : r >= -85 ? 1 : 0; }

  /* ------------------------------- API -------------------------------- */
  async function request(path, options = {}) {
    const headers = { Accept: 'application/json', ...(options.body ? { 'Content-Type': 'application/json' } : {}), ...(options.headers || {}) };
    if (state.pin) headers['X-Inverter-PIN'] = state.pin;
    const response = await fetch(path, { cache: 'no-store', ...options, headers });
    const payload = await response.json().catch(() => ({}));
    if (response.status === 401) { state.authNeeded = true; requestPin(); throw Object.assign(new Error('Security PIN required'), { auth: true }); }
    if (!response.ok) throw new Error(payload.error || payload.message || `Request failed (${response.status})`);
    state.authNeeded = false;
    return payload;
  }
  const quiet = (err) => !(err && err.auth);

  function requestPin() {
    const dlg = $('pinDialog');
    if (dlg.open) return;
    $('pinInput').value = ''; $('pinError').hidden = true;
    try { dlg.showModal(); } catch (_) { /* already open */ }
    window.setTimeout(() => $('pinInput').focus(), 40);
  }

  /* ----------------------------- rendering ----------------------------- */
  function liveState() {
    const wifiOk = Boolean(state.status && state.status.connected && state.status.got_ip);
    if (state.authNeeded) return ['warn', 'PIN required', 'Unlock to see live data'];
    if (!state.apiOk) return ['bad', 'Offline', 'Device unreachable'];
    if (state.socketOk) return ['good', 'Live', `Last update: ${Math.max(0, Math.round((Date.now() - (state.lastTelemetryAt || Date.now())) / 1000))}s ago`];
    return ['warn', 'Connecting', wifiOk ? 'Reconnecting…' : 'Waiting for network'];
  }
  function renderLive() {
    const [t, label, sub] = liveState();
    const box = $('liveBox');
    box.dataset.tone = t;
    put('live.label', label); put('live.sub', sub);
    const dotTone = t === 'good' ? 'good' : t === 'warn' ? 'warn' : 'bad';
    $$('[data-k-tone="foot.tone"]').forEach((n) => { n.className = `dot ${t === 'good' ? 'ok' : ''}`; n.dataset.tone = dotTone; });
  }

  function modeName(mode) { return isNum(mode) ? (MODE_NAMES[Number(mode)] || `Mode ${Number(mode)}`) : '—'; }
  function invLabel(s) { const t = String(s || 'unknown').replace(/_/g, ' '); return t.charAt(0).toUpperCase() + t.slice(1); }
  const STATE_RUNNING = { on: 'Running', starting: 'Starting', standby: 'Standby', off: 'Off', fault: 'Fault', diagnostic: 'Diagnostic', factory_reset: 'Resetting' };

  function renderInverter() {
    const st = state.status || {};
    const key = String(st.inverter_state || state.inverter.state || '').toLowerCase();
    const label = STATE_RUNNING[key] || invLabel(key);
    put('inv.state', label);
    $$('[data-k-tone="inv.tone"]').forEach((n) => { n.dataset.tone = key === 'on' ? 'good' : key === 'fault' ? 'bad' : key === 'starting' || key === 'standby' ? 'warn' : ''; });
    put('inv.mode', modeName(st.operating_mode ?? state.inverter.operating_mode));

    const bat = state.battery;
    const soc = isNum(bat.state_of_charge) ? Math.max(0, Math.min(100, Number(bat.state_of_charge))) : null;
    put('bat.soc', soc === null ? '—' : `${Math.round(soc)}%`);
    show('bat.est', soc !== null);
    put('bat.volts', isNum(bat.voltage) ? `${fmt(bat.voltage, ' V', 1)}` : '—');
    put('bat.cap', bat.capacity_available && isNum(bat.capacity_ah) ? `${num(bat.capacity_ah, 0)} Ah` : 'Capacity n/a');
    const lowV = isNum(bat.low_voltage_warning_v) ? Number(bat.low_voltage_warning_v) : null;
    const batTone = bat.critical ? 'bad' : bat.low ? 'warn' : (soc !== null && soc < 20 ? 'warn' : '');
    $$('[data-bar="bat"]').forEach((b) => { b.style.width = `${soc === null ? 0 : soc}%`; b.setAttribute('data-k-tone', batTone || 'good'); });
    put('bat.charge', bat.telemetry_valid === false ? '—' : bat.charging ? 'Charging' : 'Discharging / idle');
    put('bat.chargeSub', bat.low ? 'Low battery' : bat.critical ? 'Critical' : lowV ? `Warning below ${lowV.toFixed(1)} V` : 'Normal');
    put('bat.warnV', fmt(bat.low_voltage_warning_v, ' V', 1));
    put('bat.cutV', fmt(bat.cutoff_voltage_v, ' V', 1));
    put('bat.sysV', isNum(bat.system_voltage) && Number(bat.system_voltage) > 0 ? `${Number(bat.system_voltage)} V system` : '—');

    const inv = state.inverter;
    const outV = isNum(inv.voltage) && inv.telemetry_valid !== false ? Number(inv.voltage) : null;
    put('out.volt', outV === null ? '—' : `${Math.round(outV)} V`);
    show('out.chip', outV !== null);
    put('out.freq', inv.frequency_available && isNum(inv.frequency) ? `${num(inv.frequency, 1)} Hz` : 'Frequency not measured');

    const load = state.load;
    const powerKw = load.power_available && isNum(load.power_kw) ? Number(load.power_kw) : null;
    put('pow.value', powerKw === null ? 'N/A' : `${Math.round(powerKw * 1000)} W`);
    const pct = load.percentage_available && isNum(load.percentage) ? Math.max(0, Math.min(100, Number(load.percentage))) : null;
    put('pow.load', pct === null ? 'Not measured' : `${Math.round(pct)}%`);
    $$('[data-bar="load"]').forEach((b) => { b.style.width = `${pct === null ? 0 : pct}%`; });

    put('ctl.output', st.output_enabled ? 'Enabled' : 'Disabled');
    renderFlow();
  }

  function renderWifi() {
    const st = state.status || {};
    const wf = state.wifi || {};
    const connected = Boolean(st.connected && st.got_ip);
    const raw = String(st.state || wf.state || 'unknown');
    const label = connected ? 'Connected' : raw.charAt(0).toUpperCase() + raw.slice(1).replace(/_/g, ' ');
    put('wifi.state', label);
    const t = connected ? 'good' : raw === 'connecting' || raw === 'reconnecting' ? 'warn' : raw === 'failed' ? 'bad' : 'neutral';
    $$('[data-k-tone="wifi.tone"]').forEach((n) => { n.dataset.tone = t; });
    const rssi = isNum(st.rssi) ? Number(st.rssi) : (isNum(wf.rssi) ? Number(wf.rssi) : null);
    put('wifi.rssi', rssi !== null && rssi < 0 ? `${rssi} dBm` : '—');
    $$('[data-sig="wifi"]').forEach((n) => { n.dataset.level = connected ? String(sigLevel(rssi)) : '0'; });
    put('wifi.ssid', wf.ssid || st.ssid || (connected ? 'Station network' : 'No network'));
    put('wifi.ip', st.ip && st.ip !== '0.0.0.0' ? st.ip : '—');
    put('wifi.gateway', st.gateway && st.gateway !== '0.0.0.0' ? st.gateway : '—');
    put('wifi.netmask', st.netmask && st.netmask !== '0.0.0.0' ? st.netmask : '—');
    put('wifi.host', wf.hostname ? `${wf.hostname}.local` : '—');
    put('wifi.internet', st.internet_available || st.internet === 'available' ? 'Available' : connected ? 'Unavailable' : '—');
  }

  function renderSystem() {
    const sys = state.system || {};
    const st = state.status || {};
    const fw = sys.firmware_version || st.firmware_version || (state.ota && state.ota.installed_version) || '';
    put('sys.fw', fw); put('foot.fw', fw);
    put('sys.hw', sys.hardware); put('sys.device', sys.device || st.device);
    put('sys.lcd', isNum(sys.lcd_columns) ? `${sys.lcd_columns} × ${sys.lcd_rows} character LCD` : '—');
    put('sys.time', sys.system_time && sys.system_time !== 'unset' ? sys.system_time : 'Not synchronised');
    put('sys.ntp', sys.ntp_time_set ? 'Synchronised' : sys.ntp_running ? 'Waiting for sync' : 'Off');
    put('sys.heap', isNum(sys.free_heap_bytes) ? `${(sys.free_heap_bytes / 1024).toFixed(0)} KB` : '—');
    put('sys.heapMin', isNum(sys.minimum_free_heap_bytes) ? `${(sys.minimum_free_heap_bytes / 1024).toFixed(0)} KB` : '—');
    put('sys.flags', isNum(st.fault_flags) ? `0x${Number(st.fault_flags).toString(16).toUpperCase()}` : '—');
    put('sys.startErr', st.start_error && Number(st.start_error_code) ? `${st.start_error} · ${st.start_error_reason || ''}` : 'None');
    const d = state.faults.diagnostics;
    put('sys.reset', d ? d.last_reset_reason : (isNum(sys.reset_reason) ? `Code ${sys.reset_reason}` : '—'));
    put('sys.boots', d ? d.boot_count : '—');
    put('sec.state', state.authNeeded ? 'PIN required' : state.pin ? 'Unlocked in this tab' : 'No PIN entered');
    put('user.label', 'Admin');
    renderUptime();
  }
  function renderUptime() {
    if (!state.uptimeAt) return;
    const sec = state.uptimeBase + (Date.now() - state.uptimeAt) / 1000;
    put('sys.uptime', uptimeText(sec)); put('foot.uptime', uptimeText(sec));
  }
  function noteUptime(sec) { if (isNum(sec)) { state.uptimeBase = Number(sec); state.uptimeAt = Date.now(); renderUptime(); } }

  /* ----------------------------- power flow ---------------------------- */
  function renderFlow() {
    const st = state.status || {};
    const key = String(st.inverter_state || state.inverter.state || '').toLowerCase();
    const running = key === 'on';
    const solar = state.solar;
    const solarKw = solar.power_available && isNum(solar.power_kw) ? Number(solar.power_kw) : null;
    const solarOn = solarKw !== null && solarKw > 0;
    const load = state.load;
    const loadKw = load.power_available && isNum(load.power_kw) ? Number(load.power_kw) : null;
    const bat = state.battery;
    const outputOn = running && Boolean(st.output_enabled !== false);

    put('flow.solar', solarKw === null ? 'Not measured' : `${Math.round(solarKw * 1000)} W`);
    put('flow.solarSub', solar.available === true ? (solarOn ? '(Active)' : '(Idle)') : '(Not Available)');
    put('flow.inv', loadKw === null ? (running ? 'Running' : invLabel(key)) : `${Math.round(loadKw * 1000)} W`);
    put('flow.invSub', running ? '(Output)' : `(${STATE_RUNNING[key] || 'Unknown'})`);
    put('flow.load', loadKw === null ? (isNum(state.inverter.voltage) && outputOn ? `${Math.round(Number(state.inverter.voltage))} V` : 'Not measured') : `${Math.round(loadKw * 1000)} W`);
    put('flow.loadSub', outputOn ? '(Active)' : '(Standby)');
    put('flow.batt', isNum(bat.voltage) && isNum(bat.state_of_charge) ? `${num(bat.voltage, 1)} V  |  ${Math.round(Number(bat.state_of_charge))}%` : '—');
    put('flow.battSub', bat.telemetry_valid === false ? '(No data)' : bat.charging ? '(Charging)' : running ? '(Discharging)' : '(Idle)');

    $$('[data-line="solar"]').forEach((n) => n.classList.toggle('active', solarOn && running));
    $$('[data-line="load"]').forEach((n) => n.classList.toggle('active', outputOn));
    $$('[data-line="batt"]').forEach((n) => n.classList.toggle('active', Boolean(bat.charging) || running));
    $$('[data-node="solar"]').forEach((n) => { n.dataset.state = solarOn ? 'on' : 'standby'; });
    $$('[data-node="load"]').forEach((n) => { n.dataset.state = outputOn ? 'on' : 'standby'; });
    $('app').classList.toggle('no-motion', !state.motion);
  }

  /* ---------------------------- notifications --------------------------- */
  function addLive(kind, title, desc) {
    state.liveNotifs.unshift({ kind, title, desc, t: Date.now() });
    state.liveNotifs.length = Math.min(state.liveNotifs.length, 20);
    renderNotifications();
  }
  function faultTime(ev) {
    if (!state.uptimeAt || !isNum(ev.uptime_ms)) return Date.now();
    const nowUptimeMs = (state.uptimeBase + (Date.now() - state.uptimeAt) / 1000) * 1000;
    return Date.now() - Math.max(0, nowUptimeMs - Number(ev.uptime_ms));
  }
  function allNotifications() {
    const items = state.faults.events.map((ev) => {
      const sev = String(ev.severity || 'info').toLowerCase();
      const kind = sev === 'critical' || sev === 'fault' ? 'bad' : sev === 'warning' || sev === 'derated' ? 'warn' : 'info';
      const value = isNum(ev.value) ? ` (${Number(ev.value).toFixed(2)})` : '';
      return { kind, title: ev.message || 'Device event', desc: `${ev.source ? `${ev.source} · ` : ''}${sev}${value}`, t: faultTime(ev) };
    });
    return items.concat(state.liveNotifs).sort((a, b) => b.t - a.t);
  }
  const N_ICON = { info: 'i-info', ok: 'i-check', warn: 'i-info', bad: 'i-x' };
  function nodeFor(item) {
    const row = document.createElement('div'); row.className = `n-item ${item.kind}`;
    const ic = document.createElement('span'); ic.className = `n-ico ${item.kind}`;
    ic.innerHTML = `<svg class="ico"><use href="#${N_ICON[item.kind] || 'i-info'}"/></svg>`;
    const body = document.createElement('div');
    const title = document.createElement('p'); title.className = 'n-title'; title.textContent = item.title;
    const desc = document.createElement('p'); desc.className = 'n-desc'; desc.textContent = item.desc;
    body.append(title, desc);
    const time = document.createElement('span'); time.className = 'n-time'; time.textContent = ago(item.t);
    row.append(ic, body, time);
    return row;
  }
  function fill(selector, items, limit, emptyText) {
    $$(`[data-list="${selector}"]`).forEach((host) => {
      host.replaceChildren();
      const slice = items.slice(0, limit);
      if (!slice.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = emptyText; host.appendChild(p); return; }
      slice.forEach((item) => host.appendChild(nodeFor(item)));
    });
  }
  function renderNotifications() {
    const items = allNotifications();
    const attention = items.filter((i) => i.kind === 'warn' || i.kind === 'bad').length;
    put('notif.count', attention > 9 ? '9+' : String(attention));
    show('notif.count', attention > 0);
    fill('notif', items, 5, 'No notifications yet.');
    fill('notif-all', items, 100, 'No notifications yet.');
    $$('[data-list="notif-mini"]').forEach((host) => {
      host.replaceChildren();
      const slice = items.slice(0, 4);
      if (!slice.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = 'You are all caught up.'; host.appendChild(p); return; }
      slice.forEach((it) => { const d = document.createElement('div'); d.className = 'mini-n'; d.innerHTML = '<b></b><span></span>'; d.firstChild.textContent = it.title; d.lastChild.textContent = `${it.desc} · ${ago(it.t)}`; host.appendChild(d); });
    });
  }
  function renderActivity() {
    $$('[data-list="events"]').forEach((host) => {
      host.replaceChildren();
      if (!state.activity.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = 'No events yet.'; host.appendChild(p); return; }
      state.activity.forEach((a) => {
        const row = document.createElement('div'); row.className = `log-row ${a.kind}`;
        row.innerHTML = '<span class="t"></span><span class="d"></span><span></span>';
        row.children[0].textContent = hhmm(a.t); row.children[2].textContent = a.message; host.appendChild(row);
      });
    });
  }
  function renderFaults() {
    const { events, diagnostics, available } = state.faults;
    put('fault.summary', available && diagnostics ? `Boots: ${diagnostics.boot_count} · ${events.length} events` : `${events.length} events`);
    put('fault.diag', available && diagnostics
      ? `Last reset: ${diagnostics.last_reset_reason || 'unknown'} · Fault flags: 0x${Number(diagnostics.last_fault_flags || 0).toString(16).toUpperCase()} · Battery: ${fmt(diagnostics.last_battery_voltage, ' V', 2)}`
      : 'Persistent diagnostics unavailable');
    $$('[data-list="faults"]').forEach((host) => {
      host.replaceChildren();
      if (!events.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = 'No fault events have been recorded.'; host.appendChild(p); return; }
      events.forEach((ev) => {
        const sev = String(ev.severity || 'info').toLowerCase();
        const kind = sev === 'critical' || sev === 'fault' ? 'bad' : sev === 'warning' || sev === 'derated' ? 'warn' : '';
        const row = document.createElement('div'); row.className = `log-row ${kind}`;
        row.innerHTML = '<span class="t"></span><span class="d"></span><span></span>';
        row.children[0].textContent = sev.toUpperCase();
        row.children[2].textContent = `${ev.source ? `[${ev.source}] ` : ''}${ev.message || 'Unspecified fault'}${isNum(ev.value) ? ` (${Number(ev.value).toFixed(2)})` : ''}`;
        host.appendChild(row);
      });
    });
  }

  /* ------------------------------- OTA --------------------------------- */
  function renderOta() {
    const o = state.ota; if (!o) return;
    const stateName = String(o.state || 'idle');
    put('ota.state', stateName.charAt(0).toUpperCase() + stateName.slice(1));
    put('ota.installed', o.installed_version); put('ota.available', o.available_version || 'None');
    const active = o.in_progress || ['preparing', 'downloading', 'verifying'].includes(stateName);
    const pct = isNum(o.progress_percent) ? Math.max(0, Math.min(100, Number(o.progress_percent))) : 0;
    put('ota.progress', active ? `${pct}%` : '—');
    $$('[data-bar="ota"]').forEach((b) => { b.style.width = `${active ? pct : 0}%`; });
    put('ota.auto', o.auto_check_enabled ? 'On' : 'Off');
    put('ota.confirm', o.confirmation_pending ? 'Yes — awaiting confirmation' : 'No');
    put('ota.error', o.error); show('ota.error', Boolean(o.error));
    $('otaStart').disabled = !o.update_available || active;
    $('otaCancel').disabled = !active && !o.confirmation_pending;
    if (o.update_available && o.available_version && state.seen.otaVersion !== o.available_version) {
      state.seen.otaVersion = o.available_version;
      addLive('info', 'OTA Update Available', `Version ${o.available_version} is available.`);
    }
  }

  /* ----------------------------- services ------------------------------ */
  function renderServices() {
    const s = state.services; if (!s) return;
    const set = (k, on, offText = 'Offline') => $$(`[data-svc="${k}"]`).forEach((n) => { n.textContent = on ? 'Online' : offText; n.className = on ? 'on' : 'off'; });
    set('http', s.http); set('websocket', s.websocket); set('mdns', s.mdns);
    set('ntp', s.ntp_time_set, s.ntp ? 'Waiting for sync' : 'Offline');
    set('mqtt', s.mqtt_connected, s.mqtt_configured ? 'Ready' : 'Not set');
    if (s.mqtt) {
      $('mqttEnabled').checked = Boolean(s.mqtt.enabled);
      $('mqttBroker').value = s.mqtt.broker || ''; $('mqttClientId').value = s.mqtt.client_id || '';
      $('mqttUser').value = s.mqtt.username || ''; $('mqttPub').value = s.mqtt.publish_topic || ''; $('mqttSub').value = s.mqtt.subscribe_topic || '';
    }
  }

  /* ------------------------ measurement tables -------------------------- */
  function renderMeasurements() {
    const rows = [
      ['Battery voltage', isNum(state.battery.voltage) ? fmt(state.battery.voltage, ' V', 2) : 'No data', 'ADC measurement'],
      ['Battery state of charge', isNum(state.battery.state_of_charge) ? fmt(state.battery.state_of_charge, '%', 0) : 'No data', 'Firmware estimate'],
      ['AC output voltage', isNum(state.inverter.voltage) ? fmt(state.inverter.voltage, ' V', 1) : 'No data', 'ADC measurement'],
      ['Output frequency', 'Not measured', 'No sensor in this hardware'],
      ['Output current', 'Not measured', 'No current sensor'],
      ['Output power', 'Not measured', 'No current sensor'],
      ['Solar / PV', 'Not measured', 'No solar inputs configured'],
      ['Battery current / temperature', 'Not measured', 'No sensor in this hardware']
    ];
    const body = $('measureTable').tBodies[0]; body.replaceChildren();
    rows.forEach((r) => { const tr = document.createElement('tr'); r.forEach((c) => { const td = document.createElement('td'); td.textContent = c; tr.appendChild(td); }); body.appendChild(tr); });
  }
  function samplesInRange() {
    const span = RANGES[state.range];
    const cutoff = span === Infinity ? 0 : Date.now() - span;
    return state.samples.filter((s) => s.t >= cutoff);
  }
  function renderStats() {
    const list = samplesInRange();
    const body = $('statsTable').tBodies[0]; body.replaceChildren();
    [['Battery voltage', 'bat', 'V', 2], ['AC output voltage', 'out', 'V', 1]].forEach(([name, key, unit, digits]) => {
      const vals = list.map((s) => s[key]).filter((v) => Number.isFinite(v));
      const tr = document.createElement('tr');
      const cells = vals.length
        ? [name, `${Math.min(...vals).toFixed(digits)} ${unit}`, `${(vals.reduce((a, b) => a + b, 0) / vals.length).toFixed(digits)} ${unit}`, `${Math.max(...vals).toFixed(digits)} ${unit}`, String(vals.length)]
        : [name, '—', '—', '—', '0'];
      cells.forEach((c) => { const td = document.createElement('td'); td.textContent = c; tr.appendChild(td); });
      body.appendChild(tr);
    });
  }

  /* ------------------------------- charts ------------------------------- */
  const CHARTS = [
    { id: 'battery', title: 'Battery Voltage', key: 'bat', unit: 'V', min: 11, max: 14, color: '#2fd5a3', digits: 1 },
    { id: 'output', title: 'AC Output Voltage', key: 'out', unit: 'V', min: 210, max: 240, color: '#3a9bff', digits: 0 }
  ];
  function buildCharts() {
    $$('[data-slot="charts"]').forEach((host) => {
      host.replaceChildren();
      CHARTS.forEach((c) => host.appendChild(chartCard(c)));
    });
    $$('[data-chart-card]').forEach((host) => {
      const c = CHARTS.find((x) => x.id === host.dataset.chartCard);
      if (c) host.replaceChildren(...chartCard(c).childNodes);
    });
  }
  function chartCard(c) {
    const card = document.createElement('div'); card.className = 'chart-card'; card.dataset.chart = c.id;
    const head = document.createElement('div'); head.className = 'chart-head';
    const t = document.createElement('span'); t.textContent = c.title;
    const b = document.createElement('b'); b.dataset.latest = c.id; b.textContent = '—';
    head.append(t, b);
    const canvas = document.createElement('canvas'); canvas.setAttribute('role', 'img'); canvas.setAttribute('aria-label', `${c.title} trend chart`);
    card.append(head, canvas);
    return card;
  }
  function recordSample() {
    const bat = isNum(state.battery.voltage) && state.battery.telemetry_valid !== false ? Number(state.battery.voltage) : NaN;
    const out = isNum(state.inverter.voltage) && state.inverter.telemetry_valid !== false ? Number(state.inverter.voltage) : NaN;
    if (!Number.isFinite(bat) && !Number.isFinite(out)) return;
    const last = state.samples[state.samples.length - 1];
    const now = Date.now();
    if (last && now - last.t < SAMPLE_GAP_MS) { last.bat = Number.isFinite(bat) ? bat : last.bat; last.out = Number.isFinite(out) ? out : last.out; }
    else { state.samples.push({ t: now, bat, out }); if (state.samples.length > SAMPLE_CAP) state.samples.splice(0, state.samples.length - SAMPLE_CAP); }
    state.lastTelemetryAt = now;
    drawCharts();
  }
  function niceRange(lo, hi, base) {
    if (!Number.isFinite(lo)) return [base.min, base.max];
    let min = Math.min(lo, base.min), max = Math.max(hi, base.max);
    if (base.min > lo || base.max < hi) { const pad = (hi - lo) * 0.15 || 0.5; min = lo - pad; max = hi + pad; }
    return [min, max];
  }
  function drawChart(card, cfg) {
    const canvas = card.querySelector('canvas'); if (!canvas || !canvas.offsetParent) return;
    const span = RANGES[state.range];
    const now = Date.now();
    const list = samplesInRange().filter((s) => Number.isFinite(s[cfg.key]));
    const latest = list[list.length - 1];
    card.querySelector('[data-latest]').textContent = latest ? `${latest[cfg.key].toFixed(cfg.digits === 0 ? 0 : 1)} ${cfg.unit}` : '—';

    const dpr = window.devicePixelRatio || 1;
    const W = Math.max(220, canvas.clientWidth), H = Math.max(120, canvas.clientHeight);
    canvas.width = Math.round(W * dpr); canvas.height = Math.round(H * dpr);
    const ctx = canvas.getContext('2d'); if (!ctx) return;
    ctx.scale(dpr, dpr); ctx.clearRect(0, 0, W, H);
    const pad = { l: 40, r: 8, t: 8, b: 22 };
    const pw = W - pad.l - pad.r, ph = H - pad.t - pad.b;

    const vals = list.map((s) => s[cfg.key]);
    const [min, max] = niceRange(Math.min(...vals), Math.max(...vals), cfg);
    const y = (v) => pad.t + ph - ((v - min) / (max - min)) * ph;
    const t0 = span === Infinity ? (list[0] ? list[0].t : now - 60000) : now - span;
    const t1 = span === Infinity ? Math.max(now, t0 + 60000) : now;
    const x = (t) => pad.l + ((t - t0) / (t1 - t0)) * pw;

    ctx.font = '11px system-ui, sans-serif'; ctx.textBaseline = 'middle';
    ctx.strokeStyle = 'rgba(120,160,215,.14)'; ctx.fillStyle = '#8196b6'; ctx.lineWidth = 1;
    const rows = 4;
    for (let i = 0; i <= rows; i++) {
      const v = max - ((max - min) * i) / rows, yy = Math.round(pad.t + (ph * i) / rows) + 0.5;
      ctx.beginPath(); ctx.moveTo(pad.l, yy); ctx.lineTo(W - pad.r, yy); ctx.stroke();
      ctx.textAlign = 'right'; ctx.fillText(v.toFixed(cfg.digits === 0 ? 0 : 1), pad.l - 7, yy);
    }
    ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
    for (let i = 0; i <= 3; i++) { const tt = t0 + ((t1 - t0) * i) / 3; ctx.textAlign = i === 0 ? 'left' : i === 3 ? 'right' : 'center'; ctx.fillText(hhmm(tt), pad.l + (pw * i) / 3, H - 6); }
    if (!list.length) { ctx.textAlign = 'center'; ctx.fillStyle = '#6c809f'; ctx.fillText('Waiting for measurements…', pad.l + pw / 2, pad.t + ph / 2); return; }

    // downsample to roughly one point per two pixels
    const step = Math.max(1, Math.floor(list.length / (pw / 2)));
    const pts = []; for (let i = 0; i < list.length; i += step) pts.push([x(list[i].t), y(list[i][cfg.key])]);
    const lastS = list[list.length - 1]; if (pts[pts.length - 1][0] !== x(lastS.t)) pts.push([x(lastS.t), y(lastS[cfg.key])]);
    const grad = ctx.createLinearGradient(0, pad.t, 0, pad.t + ph);
    grad.addColorStop(0, `${cfg.color}55`); grad.addColorStop(1, `${cfg.color}00`);
    ctx.beginPath(); ctx.moveTo(pts[0][0], pad.t + ph); pts.forEach((p) => ctx.lineTo(p[0], p[1])); ctx.lineTo(pts[pts.length - 1][0], pad.t + ph); ctx.closePath(); ctx.fillStyle = grad; ctx.fill();
    ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])));
    ctx.strokeStyle = cfg.color; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.stroke();
    const lp = pts[pts.length - 1]; ctx.beginPath(); ctx.arc(lp[0], lp[1], 3.5, 0, Math.PI * 2); ctx.fillStyle = '#35e08a'; ctx.fill();
  }
  function drawCharts() {
    $$('.chart-card[data-chart]').forEach((card) => { const cfg = CHARTS.find((c) => c.id === card.dataset.chart); if (cfg) drawChart(card, cfg); });
    put('chart.sub', `Real-time measurements (${RANGE_LABEL[state.range]})`);
    if (!document.querySelector('[data-view="performance"]').hidden) renderStats();
  }
  function setRange(range) {
    state.range = RANGES[range] ? range : '15m'; store.set('vonix.range', state.range);
    $$('[data-range]').forEach((b) => { const on = b.dataset.range === state.range; b.classList.toggle('on', on); b.setAttribute('aria-pressed', String(on)); });
    drawCharts();
  }

  /* ----------------------------- data loading --------------------------- */
  function absorbDevice() {
    renderInverter(); renderMeasurements(); recordSample(); trackTransitions();
  }
  function trackTransitions() {
    const st = state.status || {};
    const connected = Boolean(st.connected && st.got_ip);
    if (state.seen.wifi !== null && state.seen.wifi !== connected) {
      connected ? addLive('info', 'Wi-Fi Connected', `Connected to ${(state.wifi && state.wifi.ssid) || 'network'} (RSSI: ${st.rssi} dBm).`)
        : addLive('warn', 'Wi-Fi Disconnected', 'The inverter lost its Wi-Fi connection.');
    }
    state.seen.wifi = connected;
    const inv = String(st.inverter_state || '').toLowerCase();
    if (inv && state.seen.inv !== null && state.seen.inv !== inv) {
      addLive(inv === 'fault' ? 'bad' : inv === 'on' ? 'ok' : 'info', inv === 'on' ? 'System Normal' : `Inverter ${invLabel(inv)}`, inv === 'on' ? 'Inverter operating normally.' : `Inverter state changed to ${invLabel(inv)}.`);
    }
    if (inv) state.seen.inv = inv;
    const low = Boolean(state.battery.low || state.battery.critical);
    if (low && !state.seen.batLow) addLive(state.battery.critical ? 'bad' : 'warn', state.battery.critical ? 'Battery Voltage Critical' : 'Battery Voltage Low',
      `Battery voltage dropped to ${fmt(state.battery.voltage, ' V', 1)}${isNum(state.battery.low_voltage_warning_v) ? ` (threshold: ${num(state.battery.low_voltage_warning_v, 1)} V)` : ''}.`);
    state.seen.batLow = low;
  }

  async function loadStatus() {
    try {
      const [status, wifi, inverter] = await Promise.all([request('/api/v1/status'), request('/api/v1/wifi').catch(() => ({})), request('/api/v1/inverter').catch(() => ({}))]);
      state.apiOk = true; state.status = { ...status, ...wifi, state: status.state || wifi.state }; state.wifi = wifi; state.inverter = inverter;
      noteUptime(status.uptime_seconds);
      renderWifi(); renderSystem(); absorbDevice();
    } catch (e) { if (e.auth) { state.apiOk = true; } else { state.apiOk = false; } }
    renderLive();
  }
  async function loadDevice() {
    const names = ['battery', 'solar', 'load'];
    const results = await Promise.all(names.map((n) => request(`/api/v1/${n}`).catch((e) => { if (quiet(e)) logActivity(`${n} data unavailable`, 'warn'); return null; })));
    names.forEach((n, i) => { if (results[i]) state[n] = results[i]; });
    absorbDevice();
  }
  async function loadSystem() { try { state.system = await request('/api/v1/system'); noteUptime(state.system.uptime_seconds); renderSystem(); } catch (_) { /* optional */ } }
  async function loadServices() { try { state.services = await request('/api/v1/services'); renderServices(); } catch (e) { if (quiet(e)) logActivity(e.message, 'bad'); } }
  async function loadOta() { try { state.ota = await request('/api/v1/ota'); renderOta(); renderSystem(); } catch (e) { if (quiet(e)) logActivity(`OTA status unavailable: ${e.message}`, 'warn'); } }
  async function loadFaults() {
    try {
      const p = await request('/api/v1/faults');
      state.faults = { events: Array.isArray(p.events) ? p.events : [], diagnostics: p.diagnostics || null, available: Boolean(p.diagnostics_available) };
      renderFaults(); renderNotifications(); renderSystem();
    } catch (e) { if (quiet(e)) put('fault.summary', 'Unavailable'); }
  }
  async function loadAll() { await loadStatus(); await Promise.all([loadDevice(), loadSystem(), loadServices(), loadOta(), loadFaults()]); renderLive(); }

  /* ------------------------------ WebSocket ------------------------------ */
  function send(cmd) { if (state.socket && state.socket.readyState === WebSocket.OPEN) state.socket.send(JSON.stringify(cmd)); }
  function connectSocket() {
    if (state.socket && state.socket.readyState <= WebSocket.OPEN) return;
    try { state.socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`); } catch (_) { scheduleSocket(); return; }
    const sock = state.socket;
    sock.addEventListener('open', () => { state.socketRetry = 0; send({ cmd: 'authenticate', pin: state.pin }); });
    sock.addEventListener('message', (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch (_) { return; }
      if (m.type === 'authenticated') {
        if (m.ok) { state.socketOk = true; send({ cmd: 'subscribe' }); send({ cmd: 'get_status' }); } else { requestPin(); }
        renderLive();
      } else if (m.type === 'status') {
        state.status = { ...(state.status || {}), ...m }; renderWifi(); renderInverter(); trackTransitions(); renderLive();
      } else if (m.type === 'device') { onDeviceSocket(m.data || {}); }
      else if (m.type === 'ota') { state.ota = { ...(state.ota || {}), ...(m.data || {}), in_progress: ['preparing', 'downloading', 'verifying'].includes((m.data || {}).state) }; renderOta(); }
      else if (m.type === 'error' && m.error !== 'authentication_required') { logActivity(m.error || 'Live update error', 'bad'); }
    });
    sock.addEventListener('close', () => { state.socketOk = false; renderLive(); scheduleSocket(); });
    sock.addEventListener('error', () => { state.socketOk = false; renderLive(); });
  }
  function scheduleSocket() {
    if (state.socketTimer) return;
    const delay = Math.min(30000, 1000 * 2 ** Math.min(state.socketRetry++, 5));
    state.socketTimer = window.setTimeout(() => { state.socketTimer = null; connectSocket(); }, delay);
  }
  function onDeviceSocket(d) {
    const valid = d.telemetry_valid !== false;
    state.status = { ...(state.status || {}), inverter_state: d.inverter_state ?? (state.status || {}).inverter_state, output_enabled: d.output_enabled, operating_mode: d.operating_mode, fault_flags: d.fault_flags, power_control: d.power_control || (state.status || {}).power_control };
    state.inverter = { ...state.inverter, state: d.inverter_state, voltage: valid ? d.output_voltage : null, telemetry_valid: valid, operating_mode: d.operating_mode };
    state.battery = { ...state.battery, voltage: valid ? d.battery_voltage : null, state_of_charge: valid ? d.battery_soc : null, telemetry_valid: valid,
      ...(typeof d.battery_charging === 'boolean' ? { charging: d.battery_charging } : {}),
      ...(typeof d.battery_low === 'boolean' ? { low: d.battery_low } : {}),
      ...(typeof d.battery_critical === 'boolean' ? { critical: d.battery_critical } : {}) };
    if (isNum(d.uptime_seconds)) noteUptime(d.uptime_seconds);
    absorbDevice(); renderLive();
  }

  /* --------------------------------- wifi -------------------------------- */
  function notice(text, kind = '') { const n = $('wifiNotice'); n.hidden = !text; n.className = `notice ${kind}`; n.textContent = text || ''; }
  function renderNetworks(list) {
    const host = $('netList'); host.replaceChildren();
    if (!Array.isArray(list) || !list.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = 'No visible networks found. Try scanning again.'; host.appendChild(p); return; }
    list.sort((a, b) => (b.rssi || -100) - (a.rssi || -100)).forEach((n) => {
      const row = document.createElement('div'); row.className = 'net-row';
      const info = document.createElement('div'); const b = document.createElement('b'); b.textContent = n.ssid || 'Hidden network';
      const s = document.createElement('small'); s.textContent = `${n.rssi ?? '—'} dBm · Ch ${n.channel ?? '—'} · ${n.auth || 'open'}`; info.append(b, s);
      const sig = document.createElement('span'); sig.className = 'sig'; sig.dataset.level = String(sigLevel(n.rssi)); sig.innerHTML = '<i></i><i></i><i></i><i></i>';
      const btn = document.createElement('button'); btn.type = 'button'; btn.className = 'btn'; btn.textContent = 'Connect'; btn.addEventListener('click', () => openNetwork(n));
      row.append(info, sig, btn); host.appendChild(row);
    });
  }
  async function scan() {
    const btn = $('scanBtn'); setBusy(btn, true); notice('Scanning nearby networks…');
    try { const p = await request('/api/v1/wifi/scan'); renderNetworks(p.networks); notice(''); }
    catch (e) { if (quiet(e)) { notice(e.message, 'error'); logActivity(`Network scan failed: ${e.message}`, 'bad'); } else notice(''); }
    finally { setBusy(btn, false); }
  }
  function openNetwork(n) {
    state.selectedSsid = n.ssid || '';
    $('pwTitle').textContent = `Connect to ${state.selectedSsid || 'network'}`;
    $('pwCopy').textContent = n.auth === 'open' ? 'This network is open. You can connect without a password.' : 'Your password is sent only to this inverter.';
    $('pwInput').value = ''; $('pwInput').required = n.auth !== 'open'; $('pwError').hidden = true; $('pwDialog').showModal();
  }
  async function connectWifi(ssid, password) {
    try { await request('/api/v1/wifi/connect', { method: 'POST', body: JSON.stringify({ ssid, password }) }); notice('Connecting…'); $('pwDialog').close(); window.setTimeout(loadStatus, 1200); }
    catch (e) { const el = $('pwError'); el.hidden = false; el.textContent = e.message; }
  }

  /* ----------------------------- inverter control ------------------------ */
  function openControl() {
    const pc = (state.status || {}).power_control || {};
    const supported = pc.physical_feedback_supported !== false;
    const ready = pc.interlocks_ready !== false;
    const note = $('ctlNote'); $('ctlError').hidden = true;
    note.textContent = !supported ? 'Remote power control is disabled: physical output feedback is not configured on this hardware.'
      : !ready ? `Remote power control is locked: ${pc.interlock_reason || 'interlocks not ready'}.`
      : 'Commands go through the inverter\'s own startup and protection sequence.';
    $('ctlOn').disabled = !(supported && ready); $('ctlOff').disabled = !(supported && ready);
    $('ctlDialog').showModal();
  }
  async function control(action) {
    const btn = action === 'on' ? $('ctlOn') : $('ctlOff'); setBusy(btn, true); $('ctlError').hidden = true;
    try { const r = await request('/api/v1/inverter/control', { method: 'POST', body: JSON.stringify({ action }) }); toast(r.message || `Inverter ${action}`, 'ok'); logActivity(r.message || `Inverter turned ${action}`, 'ok'); await loadStatus(); }
    catch (e) { if (quiet(e)) { const el = $('ctlError'); el.hidden = false; el.textContent = e.message; } }
    finally { setBusy(btn, false); }
  }

  /* ---------------------------------- mqtt -------------------------------- */
  async function saveMqtt(ev) {
    ev.preventDefault();
    const body = { enabled: $('mqttEnabled').checked, broker: $('mqttBroker').value.trim(), client_id: $('mqttClientId').value.trim(), username: $('mqttUser').value.trim(), publish_topic: $('mqttPub').value.trim(), subscribe_topic: $('mqttSub').value.trim(), keepalive_sec: 60, qos: 0, retain: false };
    if ($('mqttPass').value) body.password = $('mqttPass').value;
    try { await request('/api/v1/mqtt/config', { method: 'POST', body: JSON.stringify(body) }); $('mqttPass').value = ''; toast('MQTT settings saved', 'ok'); await loadServices(); }
    catch (e) { if (quiet(e)) toast(e.message, 'error'); }
  }
  async function otaAction(path) {
    try { await request(path, { method: 'POST', body: '{}' }); } catch (e) { if (quiet(e)) toast(e.message, 'error'); }
    await loadOta();
  }

  /* ---------------------------- routing & chrome ---------------------------- */
  const ROUTES = {
    dashboard: 'dashboard', live: 'dashboard', battery: 'battery', performance: 'performance', measurements: 'measurements', 'power-flow': 'power-flow',
    faults: 'faults', events: 'events', wifi: 'wifi', services: 'services', connectivity: 'connectivity', diagnostics: 'diagnostics', general: 'general',
    'battery-config': 'battery-config', security: 'security', display: 'display', firmware: 'firmware', updates: 'firmware', 'ota-settings': 'firmware', about: 'about'
  };
  const ANCHORS = { firmware: 'fw-current', updates: 'fw-updates', 'ota-settings': 'fw-settings' };
  function route() {
    const name = (location.hash.replace(/^#\/?/, '') || 'dashboard').split('?')[0];
    const key = ROUTES[name] ? name : 'dashboard';
    const view = ROUTES[key];
    $$('.view').forEach((v) => { v.hidden = v.dataset.view !== view; });
    $$('.side-link').forEach((a) => a.classList.toggle('active', a.dataset.route === key));
    $$('.dd-menu a').forEach((a) => a.classList.toggle('current', a.getAttribute('href') === `#/${key}`));
    $$('.top-btn.primary').forEach((a) => a.classList.toggle('router-active', key === 'dashboard'));
    document.title = `VONIX | ${key === 'dashboard' ? 'Inverter Management' : (document.querySelector(`.side-link[data-route="${key}"] span`) || {}).textContent || 'Dashboard'}`;
    closeMenus(); $('app').classList.remove('nav-open'); $('scrim').hidden = true; $('menuBtn').setAttribute('aria-expanded', 'false');
    if (ANCHORS[key] && $(ANCHORS[key])) $(ANCHORS[key]).scrollIntoView({ block: 'start' }); else window.scrollTo(0, 0);
    window.requestAnimationFrame(drawCharts);
    if (view === 'wifi' && !$('netList').querySelector('.net-row') && state.pin !== undefined) { /* user triggers scan */ }
  }
  function closeMenus(except) { $$('.dd.open').forEach((d) => { if (d !== except) { d.classList.remove('open'); const b = d.querySelector('[aria-expanded]'); if (b) b.setAttribute('aria-expanded', 'false'); } }); }
  function bindChrome() {
    $$('.dd').forEach((dd) => {
      const trigger = dd.querySelector('.dd-trigger, .icon-btn');
      if (!trigger) return;
      trigger.addEventListener('click', (e) => { e.stopPropagation(); const open = !dd.classList.contains('open'); closeMenus(dd); dd.classList.toggle('open', open); trigger.setAttribute('aria-expanded', String(open)); });
    });
    document.addEventListener('click', (e) => { if (!e.target.closest('.dd')) closeMenus(); });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') { closeMenus(); $('app').classList.remove('nav-open'); $('scrim').hidden = true; } });
    $('menuBtn').addEventListener('click', () => { const open = !$('app').classList.contains('nav-open'); $('app').classList.toggle('nav-open', open); $('scrim').hidden = !open; $('menuBtn').setAttribute('aria-expanded', String(open)); });
    $('scrim').addEventListener('click', () => { $('app').classList.remove('nav-open'); $('scrim').hidden = true; });
    window.addEventListener('hashchange', route);
    window.addEventListener('resize', () => window.requestAnimationFrame(drawCharts), { passive: true });

    $$('[data-range]').forEach((b) => b.addEventListener('click', () => setRange(b.dataset.range)));
    $('qaControl').addEventListener('click', openControl);
    $('ctlClose').addEventListener('click', () => $('ctlDialog').close());
    $('ctlOn').addEventListener('click', () => control('on')); $('ctlOff').addEventListener('click', () => control('off'));

    const forget = () => { state.pin = ''; session.set('inverterPin', ''); if (state.socket) state.socket.close(); renderSystem(); toast('PIN forgotten for this tab'); closeMenus(); };
    $('forgetPinBtn').addEventListener('click', forget); $('pinForgetBtn').addEventListener('click', forget);
    $('pinOpenBtn').addEventListener('click', requestPin);
    $('pinClose').addEventListener('click', () => $('pinDialog').close());
    $('pinForm').addEventListener('submit', async (e) => {
      e.preventDefault(); state.pin = $('pinInput').value.trim(); session.set('inverterPin', state.pin); $('pinDialog').close();
      await loadAll(); if (state.socket) state.socket.close(); connectSocket();
    });
    $('pwClose').addEventListener('click', () => $('pwDialog').close());
    $('pwForm').addEventListener('submit', (e) => { e.preventDefault(); connectWifi(state.selectedSsid, $('pwInput').value); });
    $('scanBtn').addEventListener('click', scan);
    $('disconnectBtn').addEventListener('click', async () => { const b = $('disconnectBtn'); setBusy(b, true); try { await request('/api/v1/wifi/disconnect', { method: 'POST', body: '{}' }); await loadStatus(); } catch (e) { if (quiet(e)) toast(e.message, 'error'); } finally { setBusy(b, false); } });
    $('refreshBtn').addEventListener('click', loadAll);
    $('mqttForm').addEventListener('submit', saveMqtt);
    $('mqttConnectBtn').addEventListener('click', async () => { const b = $('mqttConnectBtn'); setBusy(b, true); try { await request('/api/v1/mqtt/connect', { method: 'POST', body: '{}' }); await loadServices(); } catch (e) { if (quiet(e)) toast(e.message, 'error'); } finally { setBusy(b, false); } });
    $('otaCheck').addEventListener('click', () => otaAction('/api/v1/ota/check'));
    $('otaStart').addEventListener('click', () => otaAction('/api/v1/ota/start'));
    $('otaCancel').addEventListener('click', () => otaAction('/api/v1/ota/cancel'));
    $('clearLog').addEventListener('click', () => { state.activity = []; renderActivity(); });
    window.addEventListener('online', () => { loadAll(); connectSocket(); });

    $('prefRange').value = state.range;
    $('prefRange').addEventListener('change', (e) => setRange(e.target.value));
    $('prefClock24').checked = state.clock24;
    $('prefClock24').addEventListener('change', (e) => { state.clock24 = e.target.checked; store.set('vonix.clock24', state.clock24 ? '1' : '0'); tickClock(); });
    $('prefMotion').checked = state.motion;
    $('prefMotion').addEventListener('change', (e) => { state.motion = e.target.checked; store.set('vonix.motion', state.motion ? '1' : '0'); renderFlow(); });
  }

  function tickClock() { put('clock', clockText(new Date())); }

  function init() {
    $$('[data-slot="flow"]').forEach((host) => host.appendChild($('tplFlow').content.cloneNode(true)));
    buildCharts(); bindChrome(); setRange(state.range);
    renderNotifications(); renderActivity(); renderMeasurements(); renderStats(); renderLive(); renderSystem();
    route(); tickClock();
    logActivity('Dashboard loaded');
    window.setInterval(() => { tickClock(); renderUptime(); renderLive(); }, 1000);
    window.setInterval(renderNotifications, 30000);
    loadAll().then(connectSocket);
    window.setInterval(() => { loadStatus(); if (!state.socketOk) loadDevice(); }, 10000);
    window.setInterval(() => { loadServices(); loadOta(); loadFaults(); loadSystem(); }, 30000);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();
