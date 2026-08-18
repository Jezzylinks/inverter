(() => {
  'use strict';

  const state = {
    pin: sessionStorage.getItem('inverterPin') || '',
    socket: null,
    socketRetry: 0,
    socketTimer: null,
    statusTimer: null,
    reconnectTimer: null,
    selectedSsid: '',
    status: null,
    services: null
  };

  const $ = (id) => document.getElementById(id);
  const els = {
    connectionBadge: $('connectionBadge'), connectionLabel: $('connectionLabel'), pinButton: $('pinButton'),
    heroTitle: $('heroTitle'), heroSubtitle: $('heroSubtitle'), signalLabel: $('signalLabel'), signalBars: document.querySelector('.signal-bars'),
    wifiState: $('wifiState'), wifiSsid: $('wifiSsid'), deviceIp: $('deviceIp'), deviceHost: $('deviceHost'), internetState: $('internetState'), lastUpdate: $('lastUpdate'),
    scanButton: $('scanButton'), disconnectButton: $('disconnectButton'), refreshButton: $('refreshButton'), networkNotice: $('networkNotice'), networkList: $('networkList'),
    serviceHttp: $('serviceHttp'), serviceWs: $('serviceWs'), serviceMdns: $('serviceMdns'), serviceNtp: $('serviceNtp'), serviceMqtt: $('serviceMqtt'), servicePulse: $('servicePulse'),
    mqttToggleButton: $('mqttToggleButton'), mqttPanel: $('mqttPanel'), mqttCloseButton: $('mqttCloseButton'), mqttForm: $('mqttForm'), mqttConnectButton: $('mqttConnectButton'),
    mqttEnabled: $('mqttEnabled'), mqttBroker: $('mqttBroker'), mqttClientId: $('mqttClientId'), mqttUsername: $('mqttUsername'), mqttPassword: $('mqttPassword'), mqttPublishTopic: $('mqttPublishTopic'), mqttSubscribeTopic: $('mqttSubscribeTopic'),
    eventLog: $('eventLog'), clearLogButton: $('clearLogButton'), toastRegion: $('toastRegion'),
    inverterState: $('inverterState'), inverterDetail: $('inverterDetail'), batteryMetric: $('batteryMetric'), batteryDetail: $('batteryDetail'),
    solarMetric: $('solarMetric'), solarDetail: $('solarDetail'), loadMetric: $('loadMetric'), loadDetail: $('loadDetail'),
    otaState: $('otaState'), otaInstalled: $('otaInstalled'), otaAvailable: $('otaAvailable'), otaProgress: $('otaProgress'), otaCheckButton: $('otaCheckButton'), otaStartButton: $('otaStartButton'), otaCancelButton: $('otaCancelButton'),
    pinDialog: $('pinDialog'), pinForm: $('pinForm'), pinInput: $('pinInput'), pinError: $('pinError'), pinCloseButton: $('pinCloseButton'),
    passwordDialog: $('passwordDialog'), passwordForm: $('passwordForm'), passwordTitle: $('passwordTitle'), passwordCopy: $('passwordCopy'), selectedSsid: $('selectedSsid'), networkPassword: $('networkPassword'), passwordError: $('passwordError'), passwordCloseButton: $('passwordCloseButton')
  };

  function log(message, tone = '') {
    const empty = els.eventLog.querySelector('.log-empty');
    if (empty) empty.remove();
    const row = document.createElement('div');
    row.className = `event-item ${tone}`;
    const time = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    row.innerHTML = `<span class="event-time"></span><span class="event-dot"></span><span class="event-message"></span>`;
    row.querySelector('.event-time').textContent = time;
    row.querySelector('.event-message').textContent = message;
    els.eventLog.prepend(row);
    while (els.eventLog.children.length > 24) els.eventLog.lastElementChild.remove();
  }

  function toast(message, tone = '') {
    const item = document.createElement('div');
    item.className = `toast ${tone}`;
    item.textContent = message;
    els.toastRegion.appendChild(item);
    window.setTimeout(() => item.remove(), 4200);
  }

  function setNotice(message, tone = '') {
    els.networkNotice.hidden = !message;
    els.networkNotice.className = `inline-notice ${tone ? `notice-${tone}` : ''}`;
    els.networkNotice.textContent = message || '';
  }

  function setBusy(button, busy) {
    if (!button) return;
    button.classList.toggle('is-loading', busy);
    button.disabled = busy;
  }

  function apiUrl(path) { return path; }

  async function request(path, options = {}) {
    const headers = { Accept: 'application/json', ...(options.body ? { 'Content-Type': 'application/json' } : {}), ...(options.headers || {}) };
    if (state.pin) headers['X-Inverter-PIN'] = state.pin;
    const response = await fetch(apiUrl(path), { cache: 'no-store', ...options, headers });
    const payload = await response.json().catch(() => ({}));
    if (response.status === 401) {
      requestPin();
      throw new Error('Security PIN required');
    }
    if (!response.ok) throw new Error(payload.error || payload.message || `Request failed (${response.status})`);
    return payload;
  }

  function requestPin() {
    if (!els.pinDialog.open) {
      els.pinInput.value = '';
      els.pinError.hidden = true;
      els.pinDialog.showModal();
      window.setTimeout(() => els.pinInput.focus(), 40);
    }
  }

  function setConnection(kind, label) {
    els.connectionBadge.className = `status-badge status-${kind}`;
    els.connectionLabel.textContent = label;
  }

  function signalClass(rssi) {
    if (typeof rssi !== 'number' || rssi >= 0) return '';
    if (rssi >= -60) return 'good';
    if (rssi >= -75) return 'mid';
    return 'low';
  }

  function renderStatus(status) {
    if (!status) return;
    state.status = status;
    const connected = Boolean(status.connected && status.got_ip);
    const stateText = String(status.state || 'unknown');
    const friendly = stateText.charAt(0).toUpperCase() + stateText.slice(1);
    els.wifiState.textContent = connected ? 'Connected' : friendly;
    els.wifiSsid.textContent = status.ssid || (connected ? 'Station network' : 'No network');
    els.deviceIp.textContent = status.ip && status.ip !== '0.0.0.0' ? status.ip : '—';
    els.internetState.textContent = status.internet || 'Unknown';
    els.lastUpdate.textContent = `Updated ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;
    els.heroTitle.textContent = connected ? 'Your inverter is online' : stateText === 'connecting' || stateText === 'reconnecting' ? 'Connecting to your network' : 'Network connection needs attention';
    els.heroSubtitle.textContent = connected ? `Live control is available at ${status.ip || 'the device address'}.` : 'Use the network controls below to scan or connect.';
    els.signalLabel.textContent = typeof status.rssi === 'number' && status.rssi < 0 ? `${status.rssi} dBm` : '— dBm';
    els.signalBars.className = `signal-bars ${signalClass(status.rssi)}`;
    if (connected) {
      setConnection('good', 'Live connection');
    } else if (stateText === 'connecting' || stateText === 'reconnecting') {
      setConnection('warn', friendly);
    } else if (stateText === 'failed') {
      setConnection('bad', 'Connection failed');
    } else {
      setConnection('neutral', friendly);
    }
  }

  function formatValue(value, suffix = '') {
    return value === null || value === undefined || Number.isNaN(Number(value)) ? '—' : `${value}${suffix}`;
  }

  function renderDeviceData(payloads) {
    const inverter = payloads.inverter || {};
    const battery = payloads.battery || {};
    const solar = payloads.solar || {};
    const load = payloads.load || {};
    const inverterValid = inverter.telemetry_valid !== false;
    els.inverterState.textContent = inverter.state || 'Unknown';
    els.inverterDetail.textContent = inverterValid ? `${formatValue(inverter.voltage, ' V')} · ${formatValue(inverter.frequency, ' Hz')}` : 'Telemetry unavailable';
    els.batteryMetric.textContent = formatValue(battery.state_of_charge, '%');
    els.batteryDetail.textContent = battery.telemetry_valid === false ? 'Telemetry unavailable' : `${formatValue(battery.voltage, ' V')} · ${battery.charging ? 'Charging' : 'Discharging / idle'}`;
    els.solarMetric.textContent = formatValue(solar.power_kw, ' kW');
    els.solarDetail.textContent = solar.telemetry_valid === false ? 'Telemetry unavailable' : `${formatValue(solar.voltage, ' V')} · ${formatValue(solar.current, ' A')}`;
    els.loadMetric.textContent = formatValue(load.power_kw, ' kW');
    els.loadDetail.textContent = load.telemetry_valid === false ? 'Telemetry unavailable' : `${formatValue(load.percentage, '%')} · ${load.active ? 'Active' : 'Idle'}`;
  }

  function renderOta(payload) {
    if (!payload) return;
    els.otaState.textContent = payload.state || 'Unknown';
    els.otaInstalled.textContent = payload.installed_version || '—';
    els.otaAvailable.textContent = payload.available_version || '—';
    els.otaProgress.textContent = payload.in_progress || ['downloading', 'verifying', 'preparing'].includes(payload.state) ? `${payload.progress_percent ?? 0}%` : '—';
    els.otaStartButton.disabled = !payload.update_available || payload.in_progress;
    els.otaCancelButton.disabled = !payload.in_progress && !payload.confirmation_pending;
  }

  function renderDeviceSocket(message) {
    const data = message.data || {};
    renderDeviceData({
      inverter: { state: data.inverter_state, telemetry_valid: data.telemetry_valid, voltage: data.output_voltage, frequency: data.output_frequency },
      battery: { state_of_charge: data.battery_soc, telemetry_valid: data.telemetry_valid, voltage: data.battery_voltage, charging: false },
      solar: { power_kw: data.solar_voltage !== null && data.solar_current !== null ? (data.solar_voltage * data.solar_current) / 1000 : null, telemetry_valid: data.telemetry_valid, voltage: data.solar_voltage, current: data.solar_current },
      load: { power_kw: data.output_voltage !== null && data.output_current !== null ? (data.output_voltage * data.output_current) / 1000 : null, telemetry_valid: data.telemetry_valid, percentage: data.load_percentage, active: data.output_enabled }
    });
  }

  function renderServiceState(element, active, label = 'Offline') {
    element.textContent = active ? 'Online' : label;
    element.className = `service-state ${active ? 'online' : 'offline'}`;
  }

  function renderServices(payload) {
    if (!payload) return;
    state.services = payload;
    renderServiceState(els.serviceHttp, payload.http);
    renderServiceState(els.serviceWs, payload.websocket);
    renderServiceState(els.serviceMdns, payload.mdns);
    renderServiceState(els.serviceNtp, payload.ntp_time_set, payload.ntp ? 'Waiting for sync' : 'Offline');
    renderServiceState(els.serviceMqtt, payload.mqtt_connected, payload.mqtt_configured ? 'Ready' : 'Not set');
    els.servicePulse.classList.toggle('active', Boolean(payload.http && payload.websocket));
    if (payload.mqtt) {
      const mqtt = payload.mqtt;
      els.mqttEnabled.checked = Boolean(mqtt.enabled);
      els.mqttBroker.value = mqtt.broker || '';
      els.mqttClientId.value = mqtt.client_id || '';
      els.mqttUsername.value = mqtt.username || '';
      els.mqttPublishTopic.value = mqtt.publish_topic || '';
      els.mqttSubscribeTopic.value = mqtt.subscribe_topic || '';
    }
  }

  async function loadStatus(showLog = false) {
    try {
      const [status, wifi] = await Promise.all([request('/api/v1/status'), request('/api/v1/wifi')]);
      renderStatus({ ...status, ...wifi });
      if (showLog) log('Device status refreshed', 'good');
    } catch (error) {
      if (error.message !== 'Security PIN required') {
        setConnection('bad', 'API unavailable');
        if (showLog) log(error.message, 'error');
      }
    }
  }

  async function loadDeviceData() {
    const endpoints = ['inverter', 'battery', 'solar', 'load'];
    const values = await Promise.all(endpoints.map(async (name) => {
      try { return [name, await request(`/api/v1/${name}`)]; }
      catch (error) { if (error.message !== 'Security PIN required') log(`${name} data unavailable`, 'warn'); return [name, {}]; }
    }));
    renderDeviceData(Object.fromEntries(values));
  }

  async function loadOta() {
    try { renderOta(await request('/api/v1/ota')); }
    catch (error) { if (error.message !== 'Security PIN required') log(`OTA status unavailable: ${error.message}`, 'warn'); }
  }

  async function loadServices() {
    try { renderServices(await request('/api/v1/services')); } catch (error) { if (error.message !== 'Security PIN required') log(error.message, 'error'); }
  }

  function sendSocket(command) {
    if (state.socket && state.socket.readyState === WebSocket.OPEN) state.socket.send(JSON.stringify(command));
  }

  function socketUrl() {
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
    return `${scheme}://${window.location.host}/ws`;
  }

  function connectSocket() {
    if (state.socket && (state.socket.readyState === WebSocket.OPEN || state.socket.readyState === WebSocket.CONNECTING)) return;
    try { state.socket = new WebSocket(socketUrl()); } catch (error) { scheduleSocketReconnect(); return; }
    state.socket.addEventListener('open', () => {
      state.socketRetry = 0;
      setConnection('warn', 'Authenticating live updates');
      sendSocket({ cmd: 'authenticate', pin: state.pin });
      log('Live update channel opened');
    });
    state.socket.addEventListener('message', (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (_) { return; }
      if (message.type === 'authenticated') {
        if (message.ok) {
          log('Live updates authorized', 'good');
          sendSocket({ cmd: 'subscribe' });
          sendSocket({ cmd: 'get_status' });
        } else {
          log('Live update authorization failed', 'error');
          requestPin();
        }
      } else if (message.type === 'status') {
        renderStatus({ ...state.status, ...message, connected: message.connected, got_ip: message.got_ip, internet: message.internet, rssi: message.rssi, ip: message.ip, state: message.state });
      } else if (message.type === 'device') {
        renderDeviceSocket(message);
      } else if (message.type === 'ota') {
        renderOta({ ...(message.data || {}), in_progress: ['preparing', 'downloading', 'verifying'].includes(message.data?.state) });
      } else if (message.type === 'error') {
        log(message.error || 'Live update error', 'error');
      }
    });
    state.socket.addEventListener('close', () => {
      setConnection('warn', 'Live updates reconnecting');
      scheduleSocketReconnect();
    });
    state.socket.addEventListener('error', () => log('Live update channel error', 'warn'));
  }

  function scheduleSocketReconnect() {
    if (state.socketTimer) return;
    const delay = Math.min(30000, 1000 * 2 ** Math.min(state.socketRetry++, 5));
    state.socketTimer = window.setTimeout(() => { state.socketTimer = null; connectSocket(); }, delay);
  }

  function renderNetworks(networks) {
    els.networkList.replaceChildren();
    if (!Array.isArray(networks) || networks.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'empty-state';
      empty.innerHTML = '<div class="empty-icon">⌁</div><p>No visible networks found. Try scanning again.</p>';
      els.networkList.appendChild(empty);
      return;
    }
    networks.sort((a, b) => (b.rssi || -100) - (a.rssi || -100));
    networks.forEach((network) => {
      const row = document.createElement('div');
      row.className = 'network-row';
      const lock = network.auth && network.auth !== 'open' ? '🔒' : '○';
      row.innerHTML = '<span class="network-ssid"></span><span class="network-meta"></span><span class="network-lock"></span><button class="network-connect" type="button">Connect</button>';
      row.querySelector('.network-ssid').textContent = network.ssid || 'Hidden network';
      row.querySelector('.network-meta').textContent = `${network.rssi ?? '—'} dBm · Ch ${network.channel ?? '—'} · ${network.auth || 'open'}`;
      row.querySelector('.network-lock').textContent = lock;
      row.querySelector('.network-connect').addEventListener('click', () => openNetworkDialog(network));
      els.networkList.appendChild(row);
    });
  }

  async function scanNetworks() {
    setBusy(els.scanButton, true); setNotice('Scanning nearby networks…');
    try { const payload = await request('/api/v1/wifi/scan'); renderNetworks(payload.networks); setNotice(`${payload.count || 0} network${payload.count === 1 ? '' : 's'} found.`, 'good'); log(`Network scan complete: ${payload.count || 0} found`, 'good'); }
    catch (error) { setNotice(error.message, 'error'); log(`Network scan failed: ${error.message}`, 'error'); }
    finally { setBusy(els.scanButton, false); }
  }

  function openNetworkDialog(network) {
    state.selectedSsid = network.ssid || '';
    els.selectedSsid.value = state.selectedSsid;
    els.passwordTitle.textContent = `Connect to ${state.selectedSsid || 'network'}`;
    els.passwordCopy.textContent = network.auth === 'open' ? 'This network is open. You can connect without a password.' : 'Your password is sent only to this inverter.';
    els.networkPassword.value = '';
    els.networkPassword.required = network.auth !== 'open';
    els.passwordError.hidden = true;
    els.passwordDialog.showModal();
    window.setTimeout(() => els.networkPassword.focus(), 40);
  }

  async function connectNetwork(ssid, password) {
    try {
      const payload = await request('/api/v1/wifi/connect', { method: 'POST', body: JSON.stringify({ ssid, password }) });
      setNotice(payload.message || 'Connection request accepted.', 'good');
      toast('Connection request accepted', 'good'); log(`Connecting to ${ssid}`, 'good');
      els.passwordDialog.close();
      window.setTimeout(loadStatus, 1000);
    } catch (error) { els.passwordError.hidden = false; els.passwordError.textContent = error.message; log(`Connection failed: ${error.message}`, 'error'); }
  }

  async function disconnect() {
    setBusy(els.disconnectButton, true);
    try { await request('/api/v1/wifi/disconnect', { method: 'POST', body: '{}' }); toast('Wi‑Fi disconnected', 'good'); log('Wi‑Fi disconnected', 'warn'); await loadStatus(); }
    catch (error) { toast(error.message, 'error'); log(error.message, 'error'); }
    finally { setBusy(els.disconnectButton, false); }
  }

  async function saveMqtt(event) {
    event.preventDefault();
    const body = { enabled: els.mqttEnabled.checked, broker: els.mqttBroker.value.trim(), client_id: els.mqttClientId.value.trim(), username: els.mqttUsername.value.trim(), publish_topic: els.mqttPublishTopic.value.trim(), subscribe_topic: els.mqttSubscribeTopic.value.trim(), keepalive_sec: 60, qos: 0, retain: false };
    if (els.mqttPassword.value) body.password = els.mqttPassword.value;
    try { await request('/api/v1/mqtt/config', { method: 'POST', body: JSON.stringify(body) }); els.mqttPassword.value = ''; toast('MQTT settings saved', 'good'); log('MQTT configuration saved', 'good'); await loadServices(); }
    catch (error) { toast(error.message, 'error'); log(`MQTT save failed: ${error.message}`, 'error'); }
  }

  async function connectMqtt() {
    setBusy(els.mqttConnectButton, true);
    try { const result = await request('/api/v1/mqtt/connect', { method: 'POST', body: '{}' }); toast(result.message || 'MQTT connection requested', 'good'); log('MQTT connection requested'); await loadServices(); }
    catch (error) { toast(error.message, 'error'); log(`MQTT connection failed: ${error.message}`, 'error'); }
    finally { setBusy(els.mqttConnectButton, false); }
  }

  async function otaAction(path, message) {
    try {
      const result = await request(path, { method: 'POST', body: '{}' });
      toast(result.message || message, 'good');
      log(result.message || message, 'good');
      await loadOta();
    } catch (error) {
      toast(error.message, 'error');
      log(`OTA action failed: ${error.message}`, 'error');
      await loadOta();
    }
  }

  function bind() {
    els.pinButton.addEventListener('click', requestPin);
    els.pinCloseButton.addEventListener('click', () => els.pinDialog.close());
    els.passwordCloseButton.addEventListener('click', () => els.passwordDialog.close());
    els.pinForm.addEventListener('submit', async (event) => { event.preventDefault(); state.pin = els.pinInput.value.trim(); sessionStorage.setItem('inverterPin', state.pin); els.pinDialog.close(); await loadStatus(true); await loadDeviceData(); await loadServices(); await loadOta(); if (state.socket) state.socket.close(); connectSocket(); });
    els.passwordForm.addEventListener('submit', (event) => { event.preventDefault(); connectNetwork(state.selectedSsid, els.networkPassword.value); });
    els.scanButton.addEventListener('click', scanNetworks);
    els.disconnectButton.addEventListener('click', disconnect);
    els.refreshButton.addEventListener('click', async () => { await loadStatus(true); await loadDeviceData(); await loadServices(); await loadOta(); });
    els.mqttToggleButton.addEventListener('click', () => { els.mqttPanel.hidden = !els.mqttPanel.hidden; });
    els.mqttCloseButton.addEventListener('click', () => { els.mqttPanel.hidden = true; });
    els.mqttForm.addEventListener('submit', saveMqtt);
    els.mqttConnectButton.addEventListener('click', connectMqtt);
    els.otaCheckButton.addEventListener('click', () => otaAction('/api/v1/ota/check', 'OTA check started'));
    els.otaStartButton.addEventListener('click', () => otaAction('/api/v1/ota/start', 'OTA update started'));
    els.otaCancelButton.addEventListener('click', () => otaAction('/api/v1/ota/cancel', 'OTA operation cancelled'));
    els.clearLogButton.addEventListener('click', () => { els.eventLog.innerHTML = '<div class="log-empty">No events yet.</div>'; });
    window.addEventListener('online', () => { log('Browser network available', 'good'); loadStatus(); loadDeviceData(); loadOta(); connectSocket(); });
  }

  async function start() {
    bind();
    log('Dashboard loaded');
    await loadStatus();
    await loadDeviceData();
    await loadServices();
    await loadOta();
    connectSocket();
    state.statusTimer = window.setInterval(() => { loadStatus(); loadDeviceData(); loadServices(); loadOta(); }, 15000);
  }

  start();
})();
