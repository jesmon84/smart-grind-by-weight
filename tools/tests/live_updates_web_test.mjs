// Run the embedded page's live-connection and status-polling code with fake
// browser objects. A hidden tab must stop talking to the grinder, and a
// visible one must poll slowly unless a firmware update is running.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const page = readFileSync(new URL('../../src/network/device_page.h', import.meta.url), 'utf8');
const script = page.split('<script>')[1].split('</script>')[0];
const declarationStart = script.indexOf('let ws,retry=500');
const declarations = script.slice(declarationStart, script.indexOf(';', declarationStart) + 1);
const live = script.slice(script.indexOf('function connect()'), script.indexOf('function command(action)'));
const status = script.slice(script.indexOf('async function refreshStatus()'), script.indexOf('function profileFields()'));
assert.ok(declarations.includes('livePaused=document.hidden'));
assert.ok(live.includes('visibilitychange') && status.includes('statusPollMs'));

const flush = () => new Promise(resolve => setImmediate(resolve));

function harness({hidden = false} = {}) {
  const elements = new Map(), sockets = [], timers = new Map(), requests = [];
  let nextTimer = 1, visibilityHandler = null;
  const get = id => {
    if (!elements.has(id)) elements.set(id, {className: '', textContent: '', style: {}, lastElementChild: {textContent: ''}});
    return elements.get(id);
  };
  const toasts = [];
  class WebSocket {
    constructor(url) { this.url = url; this.closed = false; sockets.push(this); }
    close() { this.closed = true; }
  }
  const document = {
    hidden,
    addEventListener(type, handler) { assert.equal(type, 'visibilitychange'); visibilityHandler = handler; }
  };
  const deviceStatus = {
    firmware: {version: '2.0.0', build: 16}, device: {model: 'Test', hardware_revision: 'v2', id: 'abc'},
    network: {ssid: 'Home', hostname: 'smartgrind', ip: '192.0.2.1', rssi: -61},
    system: {reset_reason: 'POWERON'}, display: {panel_refreshes: 12}, touch: {bus_resets: 0},
    ota: {active: false, preparing: false, failed: false, progress: 0}
  };
  const context = vm.createContext({
    $: get, WebSocket, document, location: {protocol: 'http:', host: 'grinder.test'},
    state: {releaseChecked: true, deviceStatus: null},
    setTimeout(fn, delay) { const id = nextTimer++; timers.set(id, {fn, delay}); return id; },
    clearTimeout(id) { timers.delete(id); },
    render() {}, toast(text, error = false) { toasts.push({text, error}); },
    renderOtaStatus() {}, checkForUpdate() {},
    async api(url, options) {
      requests.push(options?.method ? `${options.method} ${url}` : url);
      if (options?.method === 'POST') {
        if (context.postError) throw new Error(context.postError);
        return {accepted: true};
      }
      return deviceStatus;
    }
  });
  vm.runInContext('let otaRequestPending=false,githubInstall=null;' + declarations + live + status, context);
  const setVisible = visible => { document.hidden = !visible; visibilityHandler(); };
  const pending = delay => [...timers.values()].filter(timer => timer.delay === delay);
  return {context, get, sockets, timers, requests, toasts, deviceStatus, setVisible, pending};
}

// A visible page connects once and then polls status slowly.
{
  const h = harness();
  await flush();
  assert.equal(h.sockets.length, 1);
  assert.equal(h.sockets[0].url, 'ws://grinder.test/ws');
  assert.deepEqual(h.requests, ['/api/v1/status']);
  assert.equal(h.get('wifiSignal').textContent, '-61 dBm');
  assert.equal(h.get('lastRestart').textContent, 'POWERON');
  // This status comes from firmware without panel resets: no reset button.
  assert.equal(h.get('displayHealth').textContent, '12 panel refreshes · 0 touch recoveries');
  assert.equal(h.get('resetDisplay').style.display, 'none');
  assert.equal(h.timers.size, 1);
  assert.equal(h.pending(15000).length, 1, 'idle status polling is every fifteen seconds');

  // A dropped connection is retried while the page is visible.
  h.sockets[0].onclose();
  assert.equal(h.get('connection').lastElementChild.textContent, 'Reconnecting…');
  assert.equal(h.pending(500).length, 1);

  // Hiding the tab closes the connection and cancels every timer.
  h.sockets[0].onopen();
  h.setVisible(false);
  assert.equal(h.sockets[0].closed, true);
  assert.equal(h.timers.size, 0, 'a hidden tab must not keep polling or reconnecting');
  h.sockets[0].onclose();
  assert.equal(h.get('connection').lastElementChild.textContent, 'Paused');
  assert.equal(h.timers.size, 0);
  assert.equal(h.sockets.length, 1);
  assert.equal(h.requests.length, 1);

  // Showing it again reconnects and refreshes at once.
  h.setVisible(true);
  await flush();
  assert.equal(h.sockets.length, 2);
  assert.equal(h.requests.length, 2);
  assert.equal(h.pending(15000).length, 1);

  // The old connection closing late must not start a second reconnect loop.
  h.sockets[0].onclose();
  assert.equal(h.pending(500).length, 0);
  assert.equal(h.sockets.length, 2);

  // Becoming visible twice does not open a second connection.
  h.setVisible(true);
  await flush();
  assert.equal(h.sockets.length, 2);
}

// Status is polled seldom, and every two seconds while a firmware update is
// running: each request wakes the grinder's radio.
{
  const h = harness();
  await flush();
  assert.equal(vm.runInContext('statusPollMs()', h.context), 15000);
  h.deviceStatus.ota.active = true;
  assert.equal(vm.runInContext('statusPollMs()', h.context), 2000);
  h.deviceStatus.ota.active = false;
  h.deviceStatus.ota.preparing = true;
  assert.equal(vm.runInContext('statusPollMs()', h.context), 2000);
  h.deviceStatus.ota.preparing = false;
  vm.runInContext('otaRequestPending=true', h.context);
  assert.equal(vm.runInContext('statusPollMs()', h.context), 2000);
  vm.runInContext("otaRequestPending=false;githubInstall={tag:'v2.0.1',startedAt:0}", h.context);
  assert.equal(vm.runInContext('statusPollMs()', h.context), 2000);
  vm.runInContext('githubInstall=null', h.context);
  assert.equal(vm.runInContext('statusPollMs()', h.context), 15000);

  // Two refreshes in flight still leave a single poll scheduled.
  vm.runInContext('refreshStatus();refreshStatus()', h.context);
  await flush();
  assert.equal(h.timers.size, 1);
}

// A page opened in a background tab stays quiet until it is shown.
{
  const h = harness({hidden: true});
  await flush();
  assert.equal(h.sockets.length, 0);
  assert.equal(h.timers.size, 0);
  h.setVisible(true);
  await flush();
  assert.equal(h.sockets.length, 1);
  assert.equal(h.pending(15000).length, 1);
}

// Older firmware reports none of the added status fields.
{
  const h = harness();
  delete h.deviceStatus.system; delete h.deviceStatus.display; delete h.deviceStatus.touch;
  delete h.deviceStatus.network.rssi;
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('wifiSignal').textContent, '—');
  assert.equal(h.get('lastRestart').textContent, '—');
  assert.equal(h.get('displayHealth').textContent, '—');
  assert.equal(h.get('resetDisplay').style.display, 'none');
}

// A board whose panel can be reset shows what was done to keep it working,
// the brownout restarts counted so far, and a button that asks for a reset.
{
  const h = harness();
  Object.assign(h.deviceStatus.system, {reset_reason: 'BROWNOUT', brownout_restarts: 2});
  h.deviceStatus.display = {
    panel_on: true, brightness: 255, panel_refreshes: 40, reset_supported: true,
    frame_signal: 'running', panel_resets: {display_off: 5, frame_lost: 1, requested: 1},
    faults: 4, recoveries: 3
  };
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('lastRestart').textContent, 'BROWNOUT · 2 brownout restarts counted');
  // The resets made as the display switches off are routine and not counted
  // here; these are the ones made for a panel in trouble.
  assert.equal(h.get('displayHealth').textContent,
    'Frame signal running · 2 panel resets · 40 panel refreshes · 4 panel faults (3 recovered) · 0 touch recoveries');
  assert.equal(h.get('resetDisplay').style.display, 'inline-flex');

  await h.get('resetDisplay').onclick();
  assert.equal(h.requests.at(-1), 'POST /api/v1/display/reset');
  assert.deepEqual(h.toasts.at(-1), {text: 'Display reset requested', error: false});
  assert.equal(h.pending(2000).length, 1, 'the status is read again once the reset has run');

  // The grinder can decline, for example while the display is switched off.
  h.context.postError = 'The display is switched off.';
  await h.get('resetDisplay').onclick();
  assert.deepEqual(h.toasts.at(-1), {text: 'The display is switched off.', error: true});
  assert.equal(h.pending(2000).length, 1);

  // The record of the last brownout says what was running: a grind with the
  // motor on, or an idle grinder.
  Object.assign(h.deviceStatus.system, {last_brownout: {
    uptime_s: 125, grind_phase: 'PULSE_SETTLING', motor_on: true, wifi: 'resting',
    display_on: true, brightness: 255, cpu_mhz: 240}});
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('lastRestart').textContent,
    'BROWNOUT · 2 brownout restarts counted · last one during a grind (pulse settling), motor on, Wi-Fi resting');
  Object.assign(h.deviceStatus.system.last_brownout, {grind_phase: 'IDLE', motor_on: false, wifi: 'retry_wait'});
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('lastRestart').textContent,
    'BROWNOUT · 2 brownout restarts counted · last one while idle, motor off, Wi-Fi retry wait');
  h.deviceStatus.system.last_brownout = null;

  // One brownout restart reads as one; no faults are not listed.
  Object.assign(h.deviceStatus.system, {brownout_restarts: 1});
  Object.assign(h.deviceStatus.display, {frame_signal: 'lost', faults: 0, recoveries: 0});
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('lastRestart').textContent, 'BROWNOUT · 1 brownout restart counted');
  assert.equal(h.get('displayHealth').textContent,
    'Frame signal lost · 2 panel resets · 40 panel refreshes · 0 touch recoveries');

  // A display that is switched off is asleep: its frame signal says nothing.
  Object.assign(h.deviceStatus.display, {panel_on: false, frame_signal: 'unknown'});
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('displayHealth').textContent,
    'Display off · 2 panel resets · 40 panel refreshes · 0 touch recoveries');
}

// A board without a panel reset or frame signal gets neither the button nor
// a frame signal entry.
{
  const h = harness();
  Object.assign(h.deviceStatus.system, {brownout_restarts: 0});
  h.deviceStatus.display = {
    panel_on: true, brightness: 255, panel_refreshes: 0, reset_supported: false,
    frame_signal: 'absent', panel_resets: {display_off: 0, frame_lost: 0, requested: 0},
    faults: 0, recoveries: 0
  };
  vm.runInContext('refreshStatus()', h.context);
  await flush();
  assert.equal(h.get('lastRestart').textContent, 'POWERON');
  assert.equal(h.get('displayHealth').textContent, '0 panel resets · 0 panel refreshes · 0 touch recoveries');
  assert.equal(h.get('resetDisplay').style.display, 'none');
}

console.log('Live update tests passed (production connection and polling code; browser doubles).');
