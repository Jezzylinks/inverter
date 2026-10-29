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
    services: null,
    apiReachable: false,
    apiAuthRequired: false,
    socketConnected: false,
    lastTelemetryAt: 0,
    lastApiError: '',
    chartSamples: [],
    chartRange: '15m'
  };

  const $ = (id) => document.getElementById(id);
  const els = {
    connectionBadge: $('connectionBadge'), connectionLabel: $('connectionLabel'), pinButton: $('pinButton'),
    notificationButton: $('notificationButton'), notificationPopover: $('notificationPopover'), notificationCount: $('notificationCount'), notificationTotal: $('notificationTotal'), notificationList: $('notificationList'),
    heroTitle: $('heroTitle'), heroSubtitle: $('heroSubtitle'), signalLabel: $('signalLabel'), signalBars: document.querySelector('.signal-bars'),
    outputVoltageMetric: $('outputVoltageMetric'), outputVoltageDetail: $('outputVoltageDetail'), outputPowerMetric: $('outputPowerMetric'), outputPowerDetail: $('outputPowerDetail'), loadProgressBar: $('loadProgressBar'),
    wifiDashboardState: $('wifiDashboardState'), wifiDashboardDetail: $('wifiDashboardDetail'), dashboardNotificationList: $('dashboardNotificationList'), dashboardClock: $('dashboardClock'), topLastUpdate: $('topLastUpdate'),
    wifiState: $('wifiState'), wifiSsid: $('wifiSsid'), deviceIp: $('deviceIp'), deviceHost: $('deviceHost'), internetState: $('internetState'), lastUpdate: $('lastUpdate'),
    scanButton: $('scanButton'), disconnectButton: $('disconnectButton'), refreshButton: $('refreshButton'), networkNotice: $('networkNotice'), networkList: $('networkList'),
    serviceHttp: $('serviceHttp'), serviceWs: $('serviceWs'), serviceMdns: $('serviceMdns'), serviceNtp: $('serviceNtp'), serviceMqtt: $('serviceMqtt'), servicePulse: $('servicePulse'),
    mqttToggleButton: $('mqttToggleButton'), mqttPanel: $('mqttPanel'), mqttCloseButton: $('mqttCloseButton'), mqttForm: $('mqttForm'), mqttConnectButton: $('mqttConnectButton'),
    mqttEnabled: $('mqttEnabled'), mqttBroker: $('mqttBroker'), mqttClientId: $('mqttClientId'), mqttUsername: $('mqttUsername'), mqttPassword: $('mqttPassword'), mqttPublishTopic: $('mqttPublishTopic'), mqttSubscribeTopic: $('mqttSubscribeTopic'),
    eventLog: $('eventLog'), faultHistory: $('faultHistory'), faultSummary: $('faultSummary'), faultDiagnostics: $('faultDiagnostics'),
    clearLogButton: $('clearLogButton'), toastRegion: $('toastRegion'),
    inverterState: $('inverterState'), inverterDetail: $('inverterDetail'), batteryMetric: $('batteryMetric'), batteryDetail: $('batteryDetail'),
    solarMetric: $('solarMetric'), solarDetail: $('solarDetail'), loadMetric: $('loadMetric'), loadDetail: $('loadDetail'),
    solarFlowValue: $('solarFlowValue'), solarFlowDetail: $('solarFlowDetail'), batteryFlowValue: $('batteryFlowValue'), batteryFlowDetail: $('batteryFlowDetail'),
    inverterFlowValue: $('inverterFlowValue'), inverterFlowDetail: $('inverterFlowDetail'), loadFlowValue: $('loadFlowValue'), loadFlowDetail: $('loadFlowDetail'),
    apiHealth: $('apiHealth'), apiHealthDot: $('apiHealthDot'), liveHealth: $('liveHealth'), liveHealthDot: $('liveHealthDot'),
    telemetryHealth: $('telemetryHealth'), telemetryHealthDot: $('telemetryHealthDot'),
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

  function setHealth(element, dot, label, tone) {
    if (element) element.textContent = label;
    if (dot) dot.className = `health-indicator ${tone || 'neutral'}`;
  }

  function refreshHealthStrip() {
    const apiLabel = state.apiAuthRequired ? 'PIN required' : state.apiReachable ? 'Reachable' : 'Unavailable';
    const apiTone = state.apiAuthRequired ? 'warn' : state.apiReachable ? 'good' : 'bad';
    setHealth(els.apiHealth, els.apiHealthDot, apiLabel, apiTone);
    setHealth(els.liveHealth, els.liveHealthDot, state.socketConnected ? 'Streaming' : 'Reconnecting', state.socketConnected ? 'good' : 'warn');
    if (!state.lastTelemetryAt) {
      setHealth(els.telemetryHealth, els.telemetryHealthDot, 'No valid measurements yet', 'neutral');
      return;
    }
    const ageSeconds = Math.max(0, Math.floor((Date.now() - state.lastTelemetryAt) / 1000));
    if (ageSeconds <= 10) setHealth(els.telemetryHealth, els.telemetryHealthDot, 'Fresh · just now', 'good');
    else if (ageSeconds <= 30) setHealth(els.telemetryHealth, els.telemetryHealthDot, `Aging · ${ageSeconds}s ago`, 'warn');
    else setHealth(els.telemetryHealth, els.telemetryHealthDot, `Stale · ${ageSeconds}s ago`, 'bad');
  }

  function markTelemetryFresh() {
    state.lastTelemetryAt = Date.now();
    refreshHealthStrip();
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
    if (els.wifiDashboardState) els.wifiDashboardState.textContent = connected ? 'Connected' : friendly;
    if (els.wifiDashboardDetail) els.wifiDashboardDetail.textContent = connected ? `${status.ssid || 'Wi-Fi network'} · ${typeof status.rssi === 'number' ? `${status.rssi} dBm` : 'Signal unavailable'}` : 'No active Wi-Fi connection';
    document.querySelector('.wifi-signal-bars')?.classList.toggle('is-weak', !connected || (typeof status.rssi === 'number' && status.rssi < -75));
    els.deviceIp.textContent = status.ip && status.ip !== '0.0.0.0' ? status.ip : '—';
    els.deviceHost.textContent = status.hostname ? `${status.hostname}.local` : '—';
    els.internetState.textContent = status.internet || 'Unknown';
    const updatedAt = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    els.lastUpdate.textContent = `Updated ${updatedAt}`;
    if (els.topLastUpdate) els.topLastUpdate.textContent = `Last update: ${updatedAt}`;
    els.heroTitle.textContent = connected ? 'Your inverter is online' : stateText === 'connecting' || stateText === 'reconnecting' ? 'Connecting to your network' : 'Network connection needs attention';
    els.heroSubtitle.textContent = connected ? `Live control is available at ${status.ip || 'the device address'}.` : 'Use the network controls below to scan or connect.';
    els.signalLabel.textContent = typeof status.rssi === 'number' && status.rssi < 0 ? `${status.rssi} dBm` : '— dBm';
    els.signalBars.className = `signal-bars ${signalClass(status.rssi)}`;
    if (connected && state.socketConnected) {
      setConnection('good', 'Live connection');
    } else if (connected) {
      setConnection('warn', 'Device reachable · live updates reconnecting');
    } else if (stateText === 'connecting' || stateText === 'reconnecting') {
      setConnection('warn', friendly);
    } else if (stateText === 'failed') {
      setConnection('bad', 'Connection failed');
    } else {
      setConnection('neutral', friendly);
    }
  }

  function formatValue(value, suffix = '') {
    if (value === null || value === undefined || value === '' || (typeof value === 'number' && !Number.isFinite(value))) return '—';
    if (typeof value === 'string' && value.trim() === '') return '—';
    const numeric = Number(value);
    return Number.isFinite(numeric) ? `${numeric}${suffix}` : '—';
  }

  function renderDeviceData(payloads) {
    const inverter = payloads.inverter || {};
    const battery = payloads.battery || {};
    const solar = payloads.solar || {};
    const load = payloads.load || {};
    const inverterValid = inverter.telemetry_valid !== false;
    els.inverterState.textContent = inverter.state || 'Unknown';
    els.inverterDetail.textContent = inverterValid
      ? `${formatValue(inverter.voltage, ' V')} · ${inverter.frequency_available === true ? formatValue(inverter.frequency, ' Hz') : 'Frequency not measured'}${inverter.current_available === true ? ` · ${formatValue(inverter.current, ' A')}` : ' · Current not measured'}`
      : 'Telemetry unavailable';
    const socText = formatValue(battery.state_of_charge, '%');
    els.batteryMetric.textContent = socText === '—' ? socText : `~${socText}`;
    els.batteryDetail.textContent = battery.telemetry_valid === false
      ? 'Telemetry unavailable'
      : `${formatValue(battery.voltage, ' V')} · state of charge is estimated`;
    els.solarMetric.textContent = solar.power_available === true
      ? formatValue(solar.power_kw, ' kW') : 'Not measured';
    els.solarDetail.textContent = solar.available !== true
      ? 'No solar sensors configured'
      : `${formatValue(solar.voltage, ' V')} · ${formatValue(solar.current, ' A')}`;
    els.loadMetric.textContent = load.power_available === true
      ? formatValue(load.power_kw, ' kW') : 'Not measured';
    els.loadDetail.textContent = load.telemetry_valid === false
      ? 'Telemetry unavailable'
      : `${formatValue(load.voltage, ' V')} · ${load.current_available === true ? formatValue(load.current, ' A') : 'Current not measured'} · ${load.percentage_available === true ? formatValue(load.percentage, '%') : 'Load % unavailable'}`;
    if (els.outputVoltageMetric) els.outputVoltageMetric.textContent = inverterValid ? formatValue(inverter.voltage, ' V') : '—';
    if (els.outputVoltageDetail) els.outputVoltageDetail.textContent = inverter.frequency_available === true ? `AC · ${formatValue(inverter.frequency, ' Hz')}` : 'AC output · frequency unavailable';
    const outputPowerWatts = load.power_available === true && Number.isFinite(Number(load.power_kw)) ? Number(load.power_kw) * 1000 : null;
    if (els.outputPowerMetric) els.outputPowerMetric.textContent = outputPowerWatts === null ? '—' : `${Math.round(outputPowerWatts)} W`;
    if (els.outputPowerDetail) els.outputPowerDetail.textContent = load.percentage_available === true ? `Load: ${formatValue(load.percentage, '%')}` : 'Load percentage unavailable';
    if (els.loadProgressBar) els.loadProgressBar.style.width = load.percentage_available === true ? `${Math.max(0, Math.min(100, Number(load.percentage) || 0))}%` : '0%';

    if (els.solarFlowValue) {
      els.solarFlowValue.textContent = solar.power_available === true ? formatValue(solar.power_kw, ' kW') : 'Not measured';
      els.solarFlowDetail.textContent = solar.available === true ? `${formatValue(solar.voltage, ' V')} · ${formatValue(solar.current, ' A')}` : 'Solar sensors unavailable';
      els.batteryFlowValue.textContent = socText === '—' ? '—' : `~${socText}`;
      els.batteryFlowDetail.textContent = battery.telemetry_valid === false ? 'Telemetry unavailable' : formatValue(battery.voltage, ' V');
      els.inverterFlowValue.textContent = inverter.state || 'Unknown';
      els.inverterFlowDetail.textContent = inverterValid ? formatValue(inverter.voltage, ' V') : 'Telemetry unavailable';
      els.loadFlowValue.textContent = load.power_available === true ? formatValue(load.power_kw, ' kW') : 'Not measured';
      els.loadFlowDetail.textContent = load.telemetry_valid === false ? 'Telemetry unavailable' : formatValue(load.voltage, ' V');
    }

    const hasValidMeasurement = inverter.telemetry_valid !== false &&
      [inverter.voltage, battery.voltage, battery.state_of_charge, solar.voltage, solar.current, load.voltage]
        .some((value) => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value)));
    if (hasValidMeasurement) markTelemetryFresh();
    recordChartSample(payloads);
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
    const has = (value) => value !== null && value !== undefined && value !== '';
    const solarAvailable = has(data.solar_voltage) && has(data.solar_current);
    const loadPowerAvailable = has(data.output_voltage) && has(data.output_current);
    renderDeviceData({
      inverter: {
        state: data.inverter_state,
        telemetry_valid: data.telemetry_valid,
        voltage: data.output_voltage,
        frequency: data.output_frequency,
        frequency_available: has(data.output_frequency),
        current: data.output_current,
        current_available: has(data.output_current)
      },
      battery: { state_of_charge: data.battery_soc, telemetry_valid: data.telemetry_valid, voltage: data.battery_voltage },
      solar: {
        available: solarAvailable,
        power_available: solarAvailable,
        power_kw: solarAvailable ? (Number(data.solar_voltage) * Number(data.solar_current)) / 1000 : null,
        telemetry_valid: data.telemetry_valid,
        voltage: data.solar_voltage,
        current: data.solar_current
      },
      load: {
        power_available: loadPowerAvailable,
        power_kw: loadPowerAvailable ? (Number(data.output_voltage) * Number(data.output_current)) / 1000 : null,
        telemetry_valid: data.telemetry_valid,
        voltage: data.output_voltage,
        current: data.output_current,
        current_available: has(data.output_current),
        percentage: data.load_percentage,
        percentage_available: has(data.load_percentage),
        active: data.output_enabled
      }
    });
    const hasMeasurements = [data.output_voltage, data.battery_voltage, data.battery_soc, data.solar_voltage, data.solar_current]
      .some((value) => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value)));
    if (hasMeasurements) markTelemetryFresh();
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

  async function loadStatus() {
    try {
      const [status, wifi] = await Promise.all([request('/api/v1/status'), request('/api/v1/wifi')]);
      state.apiReachable = true;
      state.apiAuthRequired = false;
      state.lastApiError = '';
      refreshHealthStrip();
      renderStatus({ ...status, ...wifi });
    } catch (error) {
      if (error.message === 'Security PIN required') {
        state.apiReachable = true;
        state.apiAuthRequired = true;
        refreshHealthStrip();
      } else {
        state.apiReachable = false;
        state.apiAuthRequired = false;
        state.lastApiError = error.message;
        refreshHealthStrip();
        setConnection('bad', 'Device API unavailable');
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


  function renderNotifications(events) {
    if (!els.notificationList) return;
    const items = Array.isArray(events) ? events.slice(0, 8) : [];
    const total = Array.isArray(events) ? events.length : 0;
    if (els.notificationTotal) els.notificationTotal.textContent = String(total);
    if (els.notificationCount) {
      els.notificationCount.hidden = total === 0;
      els.notificationCount.textContent = total > 9 ? '9+' : String(total);
    }
    els.notificationList.replaceChildren();
    if (els.dashboardNotificationList) els.dashboardNotificationList.replaceChildren();
    if (items.length === 0) {
      if (els.dashboardNotificationList) {
        const emptyDashboard = document.createElement('div');
        emptyDashboard.className = 'notification-empty';
        emptyDashboard.textContent = 'No device faults have been reported.';
        els.dashboardNotificationList.appendChild(emptyDashboard);
      }
      const empty = document.createElement('p');
      empty.className = 'notification-empty';
      empty.textContent = 'No device faults have been reported.';
      els.notificationList.appendChild(empty);
      return;
    }
    items.forEach((event) => {
      const severity = String(event.severity || 'info').toLowerCase();
      const row = document.createElement('div');
      row.className = 'notification-item ' + (severity === 'critical' || severity === 'fault' ? 'error' : severity === 'warning' || severity === 'derated' ? 'warn' : '');
      const dot = document.createElement('i');
      dot.setAttribute('aria-hidden', 'true');
      const copy = document.createElement('div');
      const title = document.createElement('strong');
      title.textContent = event.message || 'Device event';
      const meta = document.createElement('small');
      meta.textContent = severity + (event.source ? ' · ' + event.source : '');
      copy.append(title, meta);
      row.append(dot, copy);
      els.notificationList.appendChild(row);
      if (els.dashboardNotificationList) {
        const dashboardRow = document.createElement('article');
        const kind = severity === 'critical' || severity === 'fault' ? 'error' : severity === 'warning' || severity === 'derated' ? 'warn' : severity === 'ok' || severity === 'normal' ? 'good' : '';
        dashboardRow.className = `dashboard-notification ${kind}`;
        const stateIcon = document.createElement('span');
        stateIcon.className = 'notification-state-icon';
        stateIcon.setAttribute('aria-hidden', 'true');
        stateIcon.textContent = kind === 'error' ? '×' : kind === 'warn' ? '!' : kind === 'good' ? '✓' : 'i';
        const copy = document.createElement('div');
        const heading = document.createElement('strong');
        heading.textContent = event.message || 'Device event';
        const details = document.createElement('small');
        const value = typeof event.value === 'number' && Number.isFinite(event.value) ? ` · ${event.value.toFixed(2)}` : '';
        details.textContent = `${event.source || 'Device'}${value}`;
        const time = document.createElement('span');
        time.className = 'notification-time';
        time.textContent = severity.toUpperCase();
        copy.append(heading, details, time);
        dashboardRow.append(stateIcon, copy);
        els.dashboardNotificationList.appendChild(dashboardRow);
      }
    });
    if (els.dashboardNotificationList && items.length > 5) {
      const more = document.createElement('a');
      more.className = 'text-link dashboard-more-link';
      more.href = '#fault-history';
      more.textContent = `View all ${total} events →`;
      els.dashboardNotificationList.appendChild(more);
    }
  }

  async function loadFaultHistory() {
    try {
      const payload = await request('/api/v1/faults');
      els.faultHistory.replaceChildren();
      const events = Array.isArray(payload.events) ? payload.events : [];
      renderNotifications(events);
      if (events.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'log-empty';
        empty.textContent = 'No fault events have been recorded.';
        els.faultHistory.appendChild(empty);
      } else {
        events.forEach((event) => {
          const row = document.createElement('div');
          const severity = String(event.severity || 'info').toLowerCase();
          row.className = `event-item ${severity === 'critical' || severity === 'fault' ? 'error' : severity === 'warning' || severity === 'derated' ? 'warn' : ''}`;
          const time = document.createElement('span');
          time.className = 'event-time';
          time.textContent = severity.toUpperCase();
          const dot = document.createElement('span');
          dot.className = 'event-dot';
          const message = document.createElement('span');
          message.className = 'event-message';
          const source = event.source ? `[${event.source}] ` : '';
          const value = typeof event.value === 'number' && Number.isFinite(event.value) ? ` (${event.value.toFixed(2)})` : '';
          message.textContent = `${source}${event.message || 'Unspecified fault'}${value}`;
          row.append(time, dot, message);
          els.faultHistory.appendChild(row);
        });
      }
      const diagnostics = payload.diagnostics || {};
      els.faultSummary.textContent = payload.diagnostics_available
        ? `Boots: ${diagnostics.boot_count} · ${events.length} events`
        : `${events.length} events`;
      els.faultDiagnostics.textContent = payload.diagnostics_available
        ? `Last reset: ${diagnostics.last_reset_reason || 'unknown'} · Fault flags: 0x${Number(diagnostics.last_fault_flags || 0).toString(16).toUpperCase()} · Battery: ${formatValue(diagnostics.last_battery_voltage, ' V')}`
        : 'Persistent diagnostics unavailable';
    } catch (error) {
      if (error.message !== 'Security PIN required') {
        els.faultSummary.textContent = 'Unavailable';
        els.faultHistory.replaceChildren();
        const message = document.createElement('div');
        message.className = 'log-empty';
        message.textContent = error.message;
        els.faultHistory.appendChild(message);
      }
    }
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
      state.socketConnected = false;
      refreshHealthStrip();
      setConnection('warn', 'Authenticating live updates');
      sendSocket({ cmd: 'authenticate', pin: state.pin });
    });
    state.socket.addEventListener('message', (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (_) { return; }
      if (message.type === 'authenticated') {
        if (message.ok) {
          state.socketConnected = true;
          refreshHealthStrip();
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
      state.socketConnected = false;
      refreshHealthStrip();
      setConnection('warn', 'Live updates reconnecting');
      scheduleSocketReconnect();
    });
    state.socket.addEventListener('error', () => {
      state.socketConnected = false;
      refreshHealthStrip();
    });
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
    try { const payload = await request('/api/v1/wifi/scan'); renderNetworks(payload.networks); setNotice(''); }
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
      await request('/api/v1/wifi/connect', { method: 'POST', body: JSON.stringify({ ssid, password }) });
      setNotice('Connecting…');
      els.passwordDialog.close();
      window.setTimeout(loadStatus, 1000);
    } catch (error) { els.passwordError.hidden = false; els.passwordError.textContent = error.message; log(`Connection failed: ${error.message}`, 'error'); }
  }

  async function disconnect() {
    setBusy(els.disconnectButton, true);
    try { await request('/api/v1/wifi/disconnect', { method: 'POST', body: '{}' }); await loadStatus(); }
    catch (error) { toast(error.message, 'error'); log(error.message, 'error'); }
    finally { setBusy(els.disconnectButton, false); }
  }

  async function saveMqtt(event) {
    event.preventDefault();
    const body = { enabled: els.mqttEnabled.checked, broker: els.mqttBroker.value.trim(), client_id: els.mqttClientId.value.trim(), username: els.mqttUsername.value.trim(), publish_topic: els.mqttPublishTopic.value.trim(), subscribe_topic: els.mqttSubscribeTopic.value.trim(), keepalive_sec: 60, qos: 0, retain: false };
    if (els.mqttPassword.value) body.password = els.mqttPassword.value;
    try { await request('/api/v1/mqtt/config', { method: 'POST', body: JSON.stringify(body) }); els.mqttPassword.value = ''; await loadServices(); }
    catch (error) { toast(error.message, 'error'); log(`MQTT save failed: ${error.message}`, 'error'); }
  }

  async function connectMqtt() {
    setBusy(els.mqttConnectButton, true);
    try { await request('/api/v1/mqtt/connect', { method: 'POST', body: '{}' }); await loadServices(); }
    catch (error) { toast(error.message, 'error'); log(`MQTT connection failed: ${error.message}`, 'error'); }
    finally { setBusy(els.mqttConnectButton, false); }
  }

  async function otaAction(path) {
    try {
      /* OTA progress/result is already represented by the OTA status panel.
       * Avoid redundant success toasts; retain actionable failures. */
      await request(path, { method: 'POST', body: '{}' });
      await loadOta();
    } catch (error) {
      toast(error.message, 'error');
      log(`OTA action failed: ${error.message}`, 'error');
      await loadOta();
    }
  }


  function setupNavigation() {
    const nav = document.querySelector('.primary-nav');
    const menuToggle = $('mobileMenuToggle');
    const navLinks = $('primaryNavLinks');
    if (!nav || !navLinks) return;
    const groups = Array.from(nav.querySelectorAll('.nav-group'));
    const closeGroups = (except) => groups.forEach((group) => {
      if (group !== except) {
        group.classList.remove('is-open');
        const trigger = group.querySelector('.nav-trigger');
        if (trigger) trigger.setAttribute('aria-expanded', 'false');
      }
    });
    groups.forEach((group) => {
      const trigger = group.querySelector('.nav-trigger');
      trigger?.addEventListener('click', (event) => {
        event.stopPropagation();
        const opening = !group.classList.contains('is-open');
        closeGroups(group);
        group.classList.toggle('is-open', opening);
        trigger.setAttribute('aria-expanded', String(opening));
      });
    });
    document.addEventListener('click', (event) => {
      if (!nav.contains(event.target)) closeGroups(null);
    });
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        closeGroups(null);
        nav.classList.remove('mobile-open');
        menuToggle?.setAttribute('aria-expanded', 'false');
      }
    });
    menuToggle?.addEventListener('click', () => {
      const open = !nav.classList.contains('mobile-open');
      nav.classList.toggle('mobile-open', open);
      menuToggle.setAttribute('aria-expanded', String(open));
      menuToggle.setAttribute('aria-label', open ? 'Close navigation' : 'Open navigation');
    });
    navLinks.querySelectorAll('[data-nav-link]').forEach((link) => link.addEventListener('click', () => {
      nav.querySelectorAll('[data-nav-link]').forEach((item) => item.classList.toggle('is-active', item === link));
      closeGroups(null);
      nav.classList.remove('mobile-open');
      menuToggle?.setAttribute('aria-expanded', 'false');
    }));
    $('navPinAction')?.addEventListener('click', () => { closeGroups(null); requestPin(); });
    $('quickPinAction')?.addEventListener('click', requestPin);
    $('sidebarPinAction')?.addEventListener('click', requestPin);
    const clock = () => { if (els.dashboardClock) els.dashboardClock.textContent = new Date().toLocaleString([], { month: 'short', day: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit' }); };
    clock();
    window.setInterval(clock, 1000);
    $('navMqttAction')?.addEventListener('click', () => {
      closeGroups(null);
      els.mqttPanel.hidden = false;
      els.mqttPanel.scrollIntoView({ behavior: 'smooth', block: 'start' });
    });
    document.querySelectorAll('[data-chart-range]').forEach((button) => button.addEventListener('click', () => {
      state.chartRange = button.dataset.chartRange || '15m';
      document.querySelectorAll('[data-chart-range]').forEach((item) => {
        const selected = item === button;
        item.classList.toggle('is-selected', selected);
        item.setAttribute('aria-pressed', String(selected));
      });
      drawTelemetryCharts();
    }));
    window.addEventListener('resize', drawTelemetryCharts, { passive: true });
  }

  function recordChartSample(payloads) {
    const inverter = payloads.inverter || {};
    const battery = payloads.battery || {};
    const values = {
      time: Date.now(),
      batteryVoltage: Number(battery.voltage),
      outputVoltage: Number(inverter.voltage)
    };
    const batteryValid = battery.voltage !== null && battery.voltage !== undefined && battery.voltage !== '' && Number.isFinite(values.batteryVoltage);
    const outputValid = inverter.voltage !== null && inverter.voltage !== undefined && inverter.voltage !== '' && Number.isFinite(values.outputVoltage);
    if (batteryValid || outputValid) {
      state.chartSamples.push(values);
      state.chartSamples = state.chartSamples.filter((sample) => Date.now() - sample.time <= 24 * 60 * 60 * 1000).slice(-600);
      drawTelemetryCharts();
    }
  }

  function drawOneChart(canvasId, latestId, key, unit) {
    const canvas = $(canvasId);
    const latest = $(latestId);
    if (!canvas || !latest) return;
    const valid = state.chartSamples.filter((sample) => Number.isFinite(sample[key]) && sample[key] > 0);
    const filtered = state.chartRange === '15m'
      ? valid.filter((sample) => Date.now() - sample.time <= 15 * 60 * 1000)
      : valid;
    const last = filtered[filtered.length - 1];
    latest.innerHTML = last ? `${last[key].toFixed(1)} <small>${unit}</small>` : `— <small>${unit}</small>`;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(240, canvas.clientWidth || 300);
    const height = 150;
    canvas.width = Math.round(width * ratio);
    canvas.height = Math.round(height * ratio);
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.scale(ratio, ratio);
    ctx.clearRect(0, 0, width, height);
    const pad = { top: 12, right: 8, bottom: 22, left: 8 };
    const plotW = width - pad.left - pad.right;
    const plotH = height - pad.top - pad.bottom;
    ctx.lineWidth = 1;
    ctx.strokeStyle = 'rgba(174,205,236,.12)';
    for (let i = 0; i < 4; i++) {
      const y = pad.top + plotH * i / 3;
      ctx.beginPath(); ctx.moveTo(pad.left, y); ctx.lineTo(width - pad.right, y); ctx.stroke();
    }
    if (filtered.length === 0) return;
    const vals = filtered.map((sample) => sample[key]);
    let min = Math.min(...vals), max = Math.max(...vals);
    if (max - min < 0.5) { min -= 0.5; max += 0.5; }
    const points = filtered.map((sample, index) => ({
      x: pad.left + (filtered.length === 1 ? plotW / 2 : plotW * index / (filtered.length - 1)),
      y: pad.top + plotH - ((sample[key] - min) / (max - min)) * plotH
    }));
    const gradient = ctx.createLinearGradient(0, pad.top, 0, pad.top + plotH);
    gradient.addColorStop(0, 'rgba(85,214,232,.22)');
    gradient.addColorStop(1, 'rgba(85,214,232,0)');
    ctx.beginPath(); ctx.moveTo(points[0].x, pad.top + plotH);
    points.forEach((point) => ctx.lineTo(point.x, point.y));
    ctx.lineTo(points[points.length - 1].x, pad.top + plotH); ctx.closePath();
    ctx.fillStyle = gradient; ctx.fill();
    ctx.beginPath(); points.forEach((point, index) => index ? ctx.lineTo(point.x, point.y) : ctx.moveTo(point.x, point.y));
    ctx.strokeStyle = '#55d6e8'; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.stroke();
    const point = points[points.length - 1];
    ctx.beginPath(); ctx.arc(point.x, point.y, 3.5, 0, Math.PI * 2); ctx.fillStyle = '#5be4aa'; ctx.fill();
    ctx.fillStyle = '#68829e'; ctx.font = '10px system-ui, sans-serif';
    ctx.fillText(`${max.toFixed(1)} ${unit}`, pad.left, height - 5);
    ctx.textAlign = 'right'; ctx.fillText(`${min.toFixed(1)} ${unit}`, width - pad.right, height - 5);
  }

  function drawTelemetryCharts() {
    drawOneChart('batteryChart', 'batteryChartLatest', 'batteryVoltage', 'V');
    drawOneChart('outputChart', 'outputChartLatest', 'outputVoltage', 'V');
    const empty = $('chartEmpty');
    if (empty) empty.hidden = state.chartSamples.some((sample) => Number.isFinite(sample.batteryVoltage) || Number.isFinite(sample.outputVoltage));
  }

  function bind() {
    setupNavigation();
    els.notificationButton?.addEventListener('click', () => {
      const open = els.notificationPopover.hidden;
      els.notificationPopover.hidden = !open;
      els.notificationButton.setAttribute('aria-expanded', String(open));
    });
    document.addEventListener('click', (event) => {
      if (els.notificationPopover && !event.target.closest('.notification-wrap')) {
        els.notificationPopover.hidden = true;
        els.notificationButton?.setAttribute('aria-expanded', 'false');
      }
    });
    els.pinButton.addEventListener('click', requestPin);
    els.pinCloseButton.addEventListener('click', () => els.pinDialog.close());
    els.passwordCloseButton.addEventListener('click', () => els.passwordDialog.close());
    els.pinForm.addEventListener('submit', async (event) => { event.preventDefault(); state.pin = els.pinInput.value.trim(); sessionStorage.setItem('inverterPin', state.pin); els.pinDialog.close(); await loadStatus(); await loadDeviceData(); await loadServices(); await loadOta(); await loadFaultHistory(); if (state.socket) state.socket.close(); connectSocket(); });
    els.passwordForm.addEventListener('submit', (event) => { event.preventDefault(); connectNetwork(state.selectedSsid, els.networkPassword.value); });
    els.scanButton.addEventListener('click', scanNetworks);
    els.disconnectButton.addEventListener('click', disconnect);
    els.refreshButton.addEventListener('click', async () => { await loadStatus(); await loadDeviceData(); await loadServices(); await loadOta(); await loadFaultHistory(); });
    els.mqttToggleButton.addEventListener('click', () => { els.mqttPanel.hidden = !els.mqttPanel.hidden; });
    els.mqttCloseButton.addEventListener('click', () => { els.mqttPanel.hidden = true; });
    els.mqttForm.addEventListener('submit', saveMqtt);
    els.mqttConnectButton.addEventListener('click', connectMqtt);
    els.otaCheckButton.addEventListener('click', () => otaAction('/api/v1/ota/check'));
    els.otaStartButton.addEventListener('click', () => otaAction('/api/v1/ota/start'));
    els.otaCancelButton.addEventListener('click', () => otaAction('/api/v1/ota/cancel'));
    els.clearLogButton.addEventListener('click', () => { els.eventLog.innerHTML = '<div class="log-empty">No events yet.</div>'; });
    window.addEventListener('online', () => { loadStatus(); loadDeviceData(); loadOta(); connectSocket(); });
  }

  async function start() {
    bind();
    log('Dashboard loaded');
    await loadStatus();
    await loadDeviceData();
    await loadServices();
    await loadOta();
    await loadFaultHistory();
    connectSocket();
    state.statusTimer = window.setInterval(() => {
      loadStatus(); loadDeviceData(); loadServices(); loadOta(); loadFaultHistory();
    }, 15000);
    state.telemetryTimer = window.setInterval(refreshHealthStrip, 2500);
    refreshHealthStrip();
  }

  start();
})();
