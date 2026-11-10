// Status: the torso's attitude, the loops' health, battery, the last error, the ToF's 8x8
// distances, and every servo.
import {store, subscribe, has} from '../store.js';
import {t, L, esc, apply} from '../i18n.js';
import {attitudeHtml, showAttitude, rollPitch} from '../components/attitude.js';
import {diagramHtml} from '../components/diagram.js';

let el, unsub;
const $ = s => el.querySelector(s);
const f = (v, n = 1) => v == null ? '—' : v.toFixed(n);

function render(s) {
  showAttitude(el, s);
  const [roll, pitch] = s.imu_ok ? rollPitch(s.gravity) : [null, null];
  const rows = [
    ['status.roll_pitch', `${f(roll)}° / ${f(pitch)}°`], ['status.tilt', s.tilt_deg == null ? '—' : f(s.tilt_deg) + '°'],
    ['status.imu', s.imu_ok ? t('status.online') : t('status.offline')], ['status.feedback', f(s.feedback_hz, 0) + ' Hz'],
    ['status.inference', `${f(s.inference_hz, 0)} Hz · ${f(s.inference_ms, 2)} ms`], ['status.bus', f(s.bus_work_us / 1000) + ' ms'],
    ['status.overruns', s.bus_overruns], ['status.imu_misses', s.imu_misses],
    ['status.battery', s.battery_v ? f(s.battery_v, 2) + ' V' : '—'],
    ['status.error', s.last_error_code ? t('err.' + s.last_error_code) : '—'],
  ];
  $('#health').innerHTML = rows.map(([k, v]) => `<dt>${esc(t(k))}</dt><dd class="num">${esc(v)}</dd>`).join('');
  const info = store.info, names = L(info.motor_names) || [];
  $('#joints').innerHTML = info.motors.map((id, i) => `<tr class="${s.online.includes(id) ? '' : 'off'}">
    <td>${esc(names[i] || '')}</td><td>${id}</td><td class="num">${f(s.angles[i])}</td><td class="num">${f(s.targets[i])}</td><td class="num">${s.raw_pos[i]}</td></tr>`).join('');
  $('#duck').innerHTML = diagramHtml(i => s.online.includes(info.motors[i]) ? 'ok' : 'off');
  if (s.tof && $('#tof')) {
    // Near is red, 2 m and beyond blue; grey where nothing was measured.
    const cells = $('#tof').children;
    s.tof.mm.forEach((v, i) => {
      cells[i].style.background = v < 0 ? '' : `hsl(${Math.min(v / 2000, 1) * 220} 75% 55%)`;
      cells[i].textContent = v < 0 ? '' : Math.round(v / 10);
    });
    $('#tof-near').textContent = !s.tof.ok ? t('status.tof_off')
      : s.tof.nearest < 0 ? '—' : (s.tof.nearest / 10).toFixed(1) + ' cm';
  }
}

export function mount(root) {
  el = document.createElement('div');
  el.className = 'grid2';
  // Two columns on a wide screen: the body and the ToF, then the servos.
  el.innerHTML = `<div class="col">
  <section class="card"><h2 data-i18n="status.attitude"></h2>${attitudeHtml()}<dl id="health" class="kv"></dl></section>
  ${has('tof') ? `<section class="card"><div class="row"><h2 class="grow" data-i18n="status.tof"></h2><span data-i18n="status.tof_nearest"></span>
    <b id="tof-near" class="num"></b></div><div id="tof" class="tof">${'<i></i>'.repeat(64)}</div><p class="muted" data-i18n="status.tof_help"></p></section>` : ''}</div>
  <div class="col"><section class="card"><h2 data-i18n="status.servos"></h2><div id="duck"></div>
    <table><thead><tr><th data-i18n="status.joint"></th><th>ID</th><th data-i18n="status.angle"></th><th data-i18n="status.target"></th><th data-i18n="status.raw"></th></tr></thead>
    <tbody id="joints"></tbody></table></section></div>`;
  root.append(el);
  apply(el);
  unsub = subscribe(render);
}

export function unmount() {
  unsub();
}
