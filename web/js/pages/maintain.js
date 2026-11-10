// Maintain: the calibration wizard (docs/doitduck-design.md §10), servo tools and the calibration
// data. Calibration and servo setup hold the bus only while the page renews them (a heartbeat
// every second); leaving the page leaves them.
import {rpc} from '../rpc.js';
import {store, subscribe} from '../store.js';
import {t, L, esc, apply} from '../i18n.js';
import {toast, act} from '../components/toast.js';
import {diagramHtml} from '../components/diagram.js';

const TABS = ['wizard', 'servo', 'data'];
const STEPS = ['servos', 'imu_bias', 'zero', 'stand'];
let el, unsub, beat, tab = 'wizard', step = 0, calSteps = {}, s = null;
// The zero step's way: free (torque off, posed by hand) or hold (the servos hold targets[] with a weak force).
let found = {}, scanning = false, zeroMode = 'free', targets = [];
const $ = q => el.querySelector(q);
const names = () => L(store.info.motor_names) || store.info.motors.map(String);
const deg2raw = d => Math.round(d * 4095 / 360 * store.info.motor_sign);

// The setup mode this tab works in. Entered by its buttons; renewed every second while the
// device is in it, and left as soon as another tab or page is shown.
function want() {
  if (tab === 'servo' || (tab === 'wizard' && step === 0)) return 'servo';
  if (tab === 'wizard' && step === 2) return 'calibrate';
  return '';
}

function heartbeat() {
  const m = want();
  if (!s || document.hidden || scanning || (s.mode !== 'calibrate' && s.mode !== 'servo')) return;
  if (s.mode !== m) rpc(s.mode === 'servo' ? 'servo.exit' : 'calib.exit').catch(() => {});
  else if (m === 'servo') rpc('servo.enter').catch(() => {});
  else if (targets.length) rpc('calib.targets', {targets}).catch(() => {});
  else rpc('calib.enter').catch(() => {});
}

async function loadSteps() {
  const r = await rpc('calib.export');
  calSteps = r.calibration.steps;
  if ($('#undo')) {
    $('#undo').disabled = !r.can_undo;
    $('#cal-info').textContent = r.calibration.time
      ? t('maintain.saved_at', {time: new Date(r.calibration.time * 1000).toLocaleString()}) : t('maintain.never_saved');
  }
  if ($('#steps')) renderSteps();
}

const mark = (name, ok = true) => act(rpc('calib.mark', {step: name, ok}).then(loadSteps));

function renderSteps() {
  $('#steps').innerHTML = STEPS.map((k, i) => `<button data-step="${i}" class="${i === step ? 'on' : ''} ${calSteps[k] ? 'done' : ''}">
    ${calSteps[k] ? '✓ ' : ''}${i + 1}. ${esc(t('step.' + k))}</button>`).join('');
  if (step !== 2) targets = [];
  $('#step-body').innerHTML = [stepServos, stepImu, stepZero, stepStand][step]();
  apply($('#step-body'));
  render();
}

// ---- the four steps: their html; the live parts are filled by render() ----

const stepServos = () => `<p data-i18n="wizard.servos_help"></p><div class="split"><div id="w-duck"></div><div class="stack">
  <div class="row"><button id="w-check" class="primary" data-i18n="wizard.check"></button>
  <button id="w-scan" data-i18n="wizard.scan_all"></button><span id="w-scan-state" class="muted"></span></div>
  <ul id="w-found" class="check"></ul></div></div>`;

const stepImu = () => `<p data-i18n="wizard.imu_help"></p>
  <div class="row"><button id="w-imu" class="primary" data-i18n="wizard.measure"></button><span id="w-imu-state"></span></div>`;

// Two ways: posed by hand with torque off (one joint or all at once), or turned by the servos
// with a weak force, a slider each (factory zero ±60°), and recorded one by one. Rows are built
// once and only their numbers refreshed, so their buttons stay clickable.
const stepZero = () => {
  const hold = zeroMode === 'hold';
  return `<div class="modes two">${['free', 'hold'].map(m => `<button data-zmode="${m}" class="${m === zeroMode ? 'on' : ''}" data-i18n="wizard.zero_${m}"></button>`).join('')}</div>
  <p data-i18n="${hold ? 'wizard.zero_hold_help' : 'wizard.zero_help'}"></p>
  <div class="row"><button id="w-enter" data-i18n="${hold ? 'wizard.enter_hold' : 'wizard.enter'}"></button>
    <span class="muted" data-i18n="${hold ? 'wizard.enter_hold_help' : 'wizard.enter_help'}"></span></div>
  <div class="split"><div id="w-duck"></div><div class="stack">
  <table class="compact"><thead><tr><th>ID</th><th data-i18n="status.joint"></th><th data-i18n="wizard.offset"></th><th></th></tr></thead><tbody id="w-dev">
  ${names().map((n, i) => `<tr id="w-row${i}"><td class="num">${store.info.motors[i]}</td><td>${esc(n)}</td><td id="w-off${i}" class="num"></td>
    <td class="nowrap"><button data-capture="${i}" data-i18n="wizard.capture_one"></button> <span id="w-done${i}"></span></td></tr>
    ${hold ? `<tr class="sub"><td colspan="4"><input type="range" data-target="${i}" step="1" disabled
      min="${Math.max(0, store.info.factory_zero[i] - 683)}" max="${Math.min(4095, store.info.factory_zero[i] + 683)}"></td></tr>` : ''}`).join('')}</tbody></table>
  ${hold ? '' : '<div class="row"><button id="w-capture" class="primary" data-i18n="wizard.capture"></button></div>'}<ul id="w-warn" class="check"></ul></div></div>`;
};

const stepStand = () => `<p data-i18n="wizard.stand_help"></p>
  <div class="row"><button id="w-hold" data-i18n="mode.hold"></button><button id="w-suggest" class="primary" data-i18n="wizard.suggest"></button>
  <button id="w-level" data-i18n="wizard.level"></button><span id="w-level-state"></span></div>
  <dl id="w-measure" class="kv"></dl><div id="w-nudges"></div>
  <details><summary data-i18n="wizard.manual"></summary>
    <div class="row"><span data-i18n="wizard.step_deg"></span><select id="w-step"><option>0.5</option><option selected>1</option><option>2</option></select></div>
    <table><tbody>${names().map((n, i) => `<tr><td>${esc(n)}</td>
      <td><button data-nudge="${i}" data-dir="-1">−</button> <button data-nudge="${i}" data-dir="1">+</button></td></tr>`).join('')}</tbody></table></details>
  <div class="row"><button id="w-pass" class="primary" data-i18n="wizard.pass"></button></div>`;

async function checkServos() {
  scanning = true;
  found = {};
  try {
    await rpc('servo.enter');
    for (const id of [...store.info.motors, 200]) {
      found[id] = (await rpc('servo.op', {action: 'ping', target_id: id})).ok;
      render();
    }
  } catch (e) { toast(e); }
  scanning = false;
  const missing = store.info.motors.filter(id => !found[id]);
  if (!missing.length && found[200]) mark('servos');
}

async function scanAll(out, status) {
  scanning = true;
  const ids = [];
  try {
    await rpc('servo.enter');
    for (let id = 0; id <= 253 && scanning; id++) {
      status.textContent = t('maintain.scanning', {id, n: ids.length});
      if ((await rpc('servo.op', {action: 'ping', target_id: id})).ok) ids.push(id);
    }
    status.textContent = t('maintain.scan_done', {n: ids.length});
  } catch (e) { toast(e); }
  scanning = false;
  out(ids);
}

function render() {
  if (!s || !$('#step-body')) return;
  const info = store.info, n = names(), cfg = info.calibration || {};
  if (tab === 'wizard' && step === 0 && $('#w-found')) {
    $('#w-duck').innerHTML = diagramHtml(i => found[info.motors[i]] === undefined ? '' : found[info.motors[i]] ? 'ok' : 'bad');
    $('#w-found').innerHTML = [...info.motors, 200].filter(id => found[id] !== undefined)
      .map(id => `<li class="${found[id] ? 'ok' : 'bad'}">${id === 200 ? 'IMU' : esc(n[info.motors.indexOf(id)])} (ID ${id})</li>`).join('');
  }
  if (tab === 'wizard' && step === 1 && $('#w-imu-state')) {
    $('#w-imu-state').textContent = ['', t('wizard.measuring'), t('wizard.done'), t('wizard.imu_failed')][s.imu_cal_result] || '';
    if (s.imu_cal_result === 2 && !calSteps.imu_bias) { calSteps.imu_bias = true; mark('imu_bias'); }
  }
  if (tab === 'wizard' && step === 2 && $('#w-dev')) {
    const pose = cfg.pose_deg || [];
    const dev = info.motors.map((_, i) => (s.raw_pos[i] - deg2raw(pose[i] || 0) - info.factory_zero[i]) * 360 / 4095 * info.motor_sign);
    $('#w-duck').innerHTML = diagramHtml(i => s.mode === 'calibrate' && s.cal_done[i] ? 'ok' : '');
    dev.forEach((d, i) => {
      $('#w-off' + i).textContent = `${d >= 0 ? '+' : ''}${d.toFixed(1)}°`;
      $('#w-done' + i).textContent = s.cal_done[i] ? '✓' : '';
    });
  }
  if (tab === 'wizard' && step === 2) el.querySelectorAll('[data-target]').forEach(r => { r.disabled = !targets.length; });
  if (tab === 'wizard' && step === 3 && $('#w-level-state')) {
    $('#w-level-state').textContent = ['', t('wizard.measuring'), t('wizard.done'), t('wizard.level_failed')][s.level_result] || '';
  }
}

// Step 3: records one joint ({index}) or every joint ({}). Held by
// the servos, a joint's present position is its zero (calib.finish).
function capture(p) {
  if (zeroMode === 'hold') {
    rpc('calib.finish', p).then(async () => {
      await loadSteps();
      $('#w-warn').innerHTML = `<li class="ok">${esc(t('wizard.recorded', {name: names()[p.index]}))}</li>`;
    }).catch(toast);
    return;
  }
  rpc('calib.capture', p).then(async () => {
    await loadSteps();   // redraws the step, so the result goes in after it
    $('#w-warn').innerHTML = `<li class="ok">${esc(t('wizard.capture_ok'))}</li>`;
  }).catch(toast);
}

function wizardClick(e) {
  const b = e.target.closest('button');
  if (!b) return;
  const info = store.info;
  if (b.dataset.step !== undefined) { step = +b.dataset.step; renderSteps(); return; }
  switch (b.id) {
  case 'w-check': checkServos(); break;
  case 'w-scan': scanAll(ids => {
    const extra = ids.filter(id => !info.motors.includes(id) && id !== 200);
    $('#w-found').insertAdjacentHTML('beforeend', extra.map(id => `<li class="bad">${esc(t('wizard.extra_id', {id}))}</li>`).join(''));
  }, $('#w-scan-state')); break;
  case 'w-imu': act(rpc('calib.imu', {cmd: 1})); break;
  case 'w-enter':
    // Held, the servos start where they are: no jump.
    if (zeroMode === 'hold') {
      targets = [...s.raw_pos];
      // A joint already past the slider's range widens it, so the first drag does not jump.
      el.querySelectorAll('[data-target]').forEach(r => {
        const v = targets[+r.dataset.target];
        r.min = Math.max(0, Math.min(+r.min, v - 100));
        r.max = Math.min(4095, Math.max(+r.max, v + 100));
        r.value = v;
      });
    }
    act(rpc('calib.enter', {hold: zeroMode === 'hold'})); break;
  case 'w-capture': capture({}); break;
  case 'w-hold': act(rpc('robot.set_mode', {mode: 'hold'})); break;
  case 'w-level': act(rpc('calib.imu', {cmd: 2})); break;
  case 'w-pass': mark('stand'); break;
  case 'w-suggest': rpc('calib.suggest').then(r => {
    $('#w-measure').innerHTML = `<dt>${esc(t('status.roll_pitch'))}</dt><dd class="num">${r.roll_deg.toFixed(1)}° / ${r.pitch_deg.toFixed(1)}°</dd>`
      + r.tracking_deg.map((d, i) => `<dt>${esc(names()[i])}</dt><dd class="num">${d.toFixed(1)}°</dd>`).join('');
    $('#w-nudges').innerHTML = r.nudges.length ? `<p>${esc(t('wizard.suggested'))} ${r.nudges.map(([i, d]) => `${esc(names()[i])} ${d.toFixed(1)}°`).join(', ')}</p>
      <button id="w-apply" class="primary">${esc(t('wizard.apply'))}</button>` : `<p class="muted">${esc(t(Math.abs(r.roll_deg) <= 1 ? 'wizard.level_ok' : 'wizard.no_suggestion'))}</p>`;
    if (r.nudges.length) $('#w-apply').onclick = () => act(rpc('calib.nudge', {nudges: r.nudges}).then(() => toast(t('wizard.applied'))));
  }).catch(toast); break;
  }
  if (b.dataset.zmode) {
    zeroMode = b.dataset.zmode;
    targets = [];
    if (s && s.mode === 'calibrate') act(rpc('calib.enter'));   // either way starts with torque off
    renderSteps();
  }
  if (b.dataset.capture !== undefined) capture({index: +b.dataset.capture});
  if (b.dataset.nudge !== undefined) act(rpc('calib.nudge', {index: +b.dataset.nudge, delta: +b.dataset.dir * +$('#w-step').value}).then(() => toast(t('wizard.applied'))));
}

function tabHtml() {
  if (tab === 'wizard') return `<div id="steps" class="steps"></div><div id="step-body" class="stack"></div>`;
  if (tab === 'data') return `<p id="cal-info"></p><p data-i18n="maintain.data_help"></p>
    <div class="row"><button id="export" data-i18n="maintain.export"></button>
    <label class="row"><span data-i18n="maintain.import"></span><input id="import" type="file" accept=".json"></label>
    <button id="undo" data-i18n="maintain.undo"></button></div>`;
  return `<p data-i18n="maintain.servo_help"></p>
    <div class="row"><button id="sv-enter" class="primary" data-i18n="maintain.servo_enter"></button></div>
    <div class="row"><button id="sv-scan" data-i18n="wizard.scan_all"></button><button id="sv-stop" data-i18n="maintain.stop"></button><span id="sv-state" class="muted"></span></div>
    <div id="sv-found" class="row"></div>
    <div class="form">
      <span>ID</span><div class="row"><input id="sv-id" type="number" min="0" max="253" value="1">
        <button id="sv-read" data-i18n="maintain.read"></button><button id="sv-off" data-i18n="mode.off"></button><b id="sv-raw" class="num"></b></div>
      <span data-i18n="maintain.position"></span><div class="row"><input id="sv-pos" type="number" min="0" max="4095" value="2047"><button id="sv-goto" data-i18n="maintain.goto"></button></div>
      <span>KP</span><div class="row"><input id="sv-kp" type="number" value="32"><span>KD</span><input id="sv-kd" type="number" value="40">
        <button id="sv-gains" data-i18n="maintain.gains"></button></div>
      <span data-i18n="maintain.new_id"></span><div class="row"><input id="sv-new" type="number" min="0" max="253"><button id="sv-setid" data-i18n="maintain.set_id"></button></div>
    </div>`;
}

function showTab() {
  el.querySelectorAll('[data-tab]').forEach(b => b.classList.toggle('on', b.dataset.tab === tab));
  $('#tab').innerHTML = tabHtml();
  apply($('#tab'));
  if (tab === 'wizard' || tab === 'data') loadSteps().catch(toast);
  targets = [];
}

function tabClick(e) {
  const b = e.target.closest('button');
  if (tab === 'wizard') return wizardClick(e);
  if (!b) return;
  const id = () => +$('#sv-id').value, op = (action, extra = {}) => rpc('servo.op', {action, target_id: id(), ...extra});
  switch (b.id) {
  case 'export': rpc('calib.export').then(r => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(r.calibration, null, 2)], {type: 'application/json'}));
    a.download = 'calibration.json';
    a.click();
  }).catch(toast); break;
  case 'undo': if (window.confirm(t('maintain.confirm_undo'))) act(rpc('calib.undo').then(() => { toast(t('maintain.undone')); loadSteps(); })); break;
  case 'sv-enter': act(rpc('servo.enter')); break;
  case 'sv-scan': scanAll(ids => {
    $('#sv-found').innerHTML = ids.map(i => `<button data-id="${i}">${i}</button>`).join('');
  }, $('#sv-state')); break;
  case 'sv-stop': scanning = false; break;
  case 'sv-read': op('read').then(r => { $('#sv-raw').textContent = r.raw_pos; }).catch(toast); break;
  case 'sv-off': act(op('torque_off')); break;
  case 'sv-goto': act(op('goto', {raw_pos: +$('#sv-pos').value})); break;
  case 'sv-gains': act(op('set_gains', {kp: +$('#sv-kp').value, kd: +$('#sv-kd').value}).then(() => toast(t('maintain.gains_done')))); break;
  case 'sv-setid': act(op('set_id', {new_id: +$('#sv-new').value}).then(() => { toast(t('maintain.id_done')); $('#sv-id').value = $('#sv-new').value; })); break;
  }
  if (b.dataset.id) $('#sv-id').value = b.dataset.id;
}

export function mount(root) {
  el = document.createElement('section');
  el.className = 'card';
  el.innerHTML = `<div class="tabs">${TABS.map(k => `<button data-tab="${k}">${esc(t('maintain.' + k))}</button>`).join('')}</div><div id="tab" class="stack"></div>`;
  root.append(el);
  el.querySelector('.tabs').onclick = e => {
    const b = e.target.closest('[data-tab]');
    if (b) { scanning = false; tab = b.dataset.tab; showTab(); }
  };
  $('#tab').onclick = tabClick;
  // A zero slider: the servo follows while dragging (one request at a time), and the last value is sent on release.
  let sending = false;
  const follow = e => {
    if (!e.target.dataset.target || !targets.length) return;
    targets[+e.target.dataset.target] = +e.target.value;
    if (sending && e.type === 'input') return;
    sending = true;
    rpc('calib.targets', {targets}).catch(toast).finally(() => { sending = false; });
  };
  $('#tab').oninput = follow;
  $('#tab').onchange = e => {
    follow(e);
    if (e.target.id !== 'import' || !e.target.files[0]) return;
    e.target.files[0].text().then(text => rpc('calib.import', {calibration: JSON.parse(text)}))
      .then(() => { toast(t('maintain.imported')); loadSteps(); }).catch(toast);
  };
  unsub = subscribe(st => { s = st; render(); });
  beat = setInterval(heartbeat, 1000);
  showTab();
}

export function unmount() {
  scanning = false;
  clearInterval(beat);
  unsub();
  if (s && (s.mode === 'calibrate' || s.mode === 'servo')) rpc(s.mode === 'servo' ? 'servo.exit' : 'calib.exit').catch(() => {});
}
