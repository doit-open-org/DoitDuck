// Control: an overview (attitude, what the duck is doing, the loops and battery), the camera, the voice bar (voice, xiaozhi and gestures as one log of what was heard
// and done, quack buttons), modes, skills, the two sticks and the mouth. Motion is sent only
// while a stick or key is held, so an idle page holds no control.
import {rpc} from '../rpc.js';
import {store, subscribe, has} from '../store.js';
import {t, tr, L, esc, apply, resultText} from '../i18n.js';
import {toast, act} from '../components/toast.js';
import {joystick} from '../components/joystick.js';
import {attitudeHtml, showAttitude} from '../components/attitude.js';

let el, unsub, timer, resets = [], cam = null;
const sticks = {l: [0, 0], r: [0, 0]}, keys = new Set();
let headMode = false, moving = false, busy = false, lastHead = '';
let log = [], seen = {};

const $ = s => el.querySelector(s);
const dz = v => Math.abs(v) < 0.1 ? 0 : v;

function html() {
  const info = store.info, voice = has('voice') || has('xiaozhi');
  const skills = info.skills.map(s => `<button data-skill="${esc(s.name)}">${esc(L(s.label))}${s.verified ? '' : `<span class="badge">${esc(t('actions.unverified'))}</span>`}</button>`).join('');
  // The overview across the top; then two columns on a wide screen: what the duck sees and says, and what moves it.
  const stat = (id, k) => `<div><small>${esc(t(k))}</small><b id="${id}" class="num">—</b></div>`;
  return `<section class="card hero landscape-hide">${attitudeHtml()}
    <div class="hero-text"><small data-i18n="control.now"></small><h1 id="phase">—</h1>
      <div class="stats">${stat('c-fb', 'control.feedback')}${stat('c-rl', 'status.inference')}${stat('c-servo', 'status.servos')}${stat('c-imu', 'status.imu')}${stat('c-batt', 'status.battery')}</div></div>
  </section>
  <div class="col">
  <section class="card landscape-hide">
    <div class="row"><h2 class="grow" data-i18n="control.camera"></h2>
      ${has('gesture') ? '<label class="row"><input type="checkbox" id="gesture"><span data-i18n="control.gesture"></span></label>' : ''}
      <button id="cam-btn" data-i18n="control.camera_on"></button></div>
    <video id="cam" muted playsinline autoplay hidden></video>
  </section>
  ${voice ? `<section class="card voice landscape-hide">
    <div class="row"><span id="v-state" class="state grow"></span>
      ${has('voice') ? '<label class="row"><input type="checkbox" id="v-on"><span data-i18n="control.voice_on"></span></label>' : ''}
      <button id="v-wake" data-i18n="control.wake"></button>
      ${has('xiaozhi') ? '<button id="v-stop" data-i18n="control.interrupt" hidden></button>' : ''}</div>
    <p id="v-off" class="chip warn" data-i18n="control.voice_off_warn" hidden></p>
    <ul id="v-log" class="log"></ul>
    <div id="quacks" class="quacks"></div>
  </section>` : ''}</div>
  <div class="col">
  <section class="card landscape-hide">
    <div class="modes">${['preview', 'hold', 'walk', 'off'].map(m => `<button data-mode="${m}" data-i18n="mode.${m}"></button>`).join('')}</div>
    <h3 data-i18n="control.skills"></h3>
    <div class="skills">${skills || `<a href="#actions" data-i18n="control.no_skills"></a>`}</div>
  </section>
  <section class="card">
    <div class="row landscape-hide"><span class="grow" data-i18n="control.sticks"></span>
      <button id="stick-mode"></button></div>
    <div class="sticks"><div id="stick-l"><small id="stick-l-label"></small><b id="stick-l-v" class="num">0.00 / 0.00</b></div>
      <div id="stick-r"><small id="stick-r-label"></small><b id="stick-r-v" class="num">0.00</b></div></div>
    <label class="landscape-hide"><span data-i18n="control.mouth"></span> <b id="mouth-v" class="num">0°</b>
      <input id="mouth" type="range" min="0" max="${info.mouth_max}" step="1" value="0"></label>
    <div class="landscape-hide"><div class="row"><span class="grow" data-i18n="control.head"></span>
      <button id="head-center" data-i18n="control.head_center"></button></div>
      ${['neck', 'pitch', 'yaw', 'roll'].map((k, i) => `<label><span data-i18n="control.head_${k}"></span> <b id="head-v${i}" class="num">0.00</b>
      <input data-head="${i}" type="range" min="-1" max="1" step="0.05" value="0"></label>`).join('')}</div>
    <p class="muted landscape-hide keys-hint" data-i18n="control.keys"></p>
  </section></div>`;
}

function stickLabels() {
  $('#stick-mode').textContent = t(headMode ? 'control.mode_head' : 'control.mode_walk');
  $('#stick-l-label').textContent = t(headMode ? 'control.stick_head_l' : 'control.stick_walk_l');
  $('#stick-r-label').textContent = t(headMode ? 'control.stick_head_r' : 'control.stick_walk_r');
}

// Every 50 ms: the body's twist while something is held, or the head while in head mode.
function send() {
  if (document.hidden || busy) return;
  const vmax = store.info.vmax;
  if (headMode) {
    const head = [-sticks.r[1], -sticks.l[1], -sticks.l[0], sticks.r[0]].map(v => +dz(v).toFixed(2));
    const key = head.join();
    if (key === lastHead) return;
    lastHead = key;
    busy = true;
    rpc('robot.head', {head}).catch(toast).finally(() => busy = false);
    return;
  }
  const k = (a, b) => keys.has(a) - keys.has(b);
  const tw = [
    keys.has('w') || keys.has('s') ? k('w', 's') * 0.5 * vmax[0] : dz(sticks.l[1]) * vmax[0],
    keys.has('a') || keys.has('d') ? k('a', 'd') * 0.5 * vmax[1] : -dz(sticks.l[0]) * vmax[1],
    keys.has('q') || keys.has('e') ? k('q', 'e') * 0.5 * vmax[2] : -dz(sticks.r[0]) * vmax[2]];
  const active = tw.some(v => v !== 0);
  if (!active && !moving) return;
  moving = active;
  busy = true;
  rpc('robot.move', {vx: tw[0], vy: tw[1], wz: tw[2]}).catch(e => { toast(e); stop(); }).finally(() => busy = false);
}

function stop() {
  keys.clear();
  resets.forEach(r => r());
}

function toggleCamera() {
  if (cam) {
    cam.ws.close();
    cam.mux.destroy();
    cam = null;
    $('#cam').hidden = true;
    $('#cam-btn').textContent = t('control.camera_on');
    return;
  }
  if (!window.JMuxer) {
    const s = document.createElement('script');
    s.src = 'vendor/jmuxer.min.js';
    s.onload = toggleCamera;
    document.head.append(s);
    return;
  }
  const v = $('#cam');
  const mux = new window.JMuxer({node: v, mode: 'video', fps: 15, flushingTime: 0, maxDelay: 500});
  const ws = new WebSocket(`ws://${location.host}/video`);
  ws.binaryType = 'arraybuffer';
  let last = 0;
  ws.onmessage = e => {
    // Each frame lasts as long as it took to arrive, so playback keeps pace and no backlog builds up.
    const now = performance.now(), duration = last ? Math.min(Math.max(now - last, 20), 200) : 1000 / 15;
    last = now;
    mux.feed({video: new Uint8Array(e.data), duration});
    const b = v.buffered, behind = b.length ? b.end(b.length - 1) - v.currentTime : 0;
    v.playbackRate = behind > 0.3 ? 1.25 : 1;
  };
  ws.onclose = () => { if (cam && cam.ws === ws) toggleCamera(); };
  cam = {ws, mux};
  v.hidden = false;
  $('#cam-btn').textContent = t('control.camera_off');
}

// The voice bar's log: a new line whenever what was heard, said or done changes.
function remember(s) {
  const add = (who, text) => {
    log.unshift({who, text});
    log = log.slice(0, 8);
  };
  const v = s.voice, x = s.xiaozhi, g = s.gesture;
  if (v && v.last_words && (v.last_words + v.last_result) !== seen.voice) {
    seen.voice = v.last_words + v.last_result;
    add('you', `🎤 ${v.last_words}${v.last_score ? ' ' + v.last_score.toFixed(2) : ''} → ${resultText(v.last_result)}`);
  }
  if (x && x.stt && x.stt !== seen.stt) add('you', t('control.you_said', {text: seen.stt = x.stt}));
  if (x && x.tts && x.tts !== seen.tts) add('duck', t('control.duck_said', {text: seen.tts = x.tts}));
  // Interrupts since the page opened (the count it opened with is not news).
  if (x && x.interrupts !== seen.interrupts) {
    if (seen.interrupts !== undefined) add('you', t('control.interrupted'));
    seen.interrupts = x.interrupts;
  }
  if (g && g.last_call && (g.last_call + g.last_result + g.gesture) !== seen.gesture) {
    seen.gesture = g.last_call + g.last_result + g.gesture;
    add('you', `✋ ${t('gesture.' + (g.gesture || 'none'))} → ${g.last_call} → ${resultText(g.last_result)}`);
  }
  return log.map(l => `<li class="who-${l.who}">${esc(l.text)}</li>`).join('');
}

function render(s) {
  document.querySelectorAll('[data-mode]').forEach(b => {
    b.classList.toggle('on', b.dataset.mode === s.mode);
    if (b.dataset.mode === 'hold' || b.dataset.mode === 'walk') b.disabled = !s.control_ready;
  });
  const skill = s.skill && store.info.skills.find(k => k.name === s.skill);
  $('#phase').textContent = s.last_error_code ? t('err.' + s.last_error_code)
    : skill ? t('control.doing', {name: L(skill.label), pct: (s.skill_progress * 100).toFixed(0)})
    : !s.imu_ok ? t('control.imu_offline') : t('phase.' + s.phase);
  $('#phase').classList.toggle('bad', !!s.last_error_code || !s.imu_ok);
  showAttitude(el, s);
  const n = store.info.motors.length;
  $('#c-fb').textContent = s.feedback_hz.toFixed(0) + ' Hz';
  $('#c-rl').textContent = s.inference_hz.toFixed(0) + ' Hz';
  $('#c-servo').textContent = `${s.online.length}/${n}`;
  $('#c-servo').classList.toggle('bad', s.online.length < n);
  $('#c-imu').textContent = t(s.imu_ok ? 'status.online' : 'status.offline');
  $('#c-imu').classList.toggle('bad', !s.imu_ok);
  $('#c-batt').textContent = s.battery_v ? s.battery_v.toFixed(2) + ' V' : '—';
  $('#c-batt').classList.toggle('bad', s.battery_v > 0 && s.battery_v < 6.8);
  // The head sliders show where the head is (the stick and gestures move it too), except the one being dragged.
  el.querySelectorAll('[data-head]').forEach((r, i) => {
    if (s.head && !r.matches(':active')) r.value = s.head[i];
    $('#head-v' + i).textContent = (+r.value).toFixed(2);
  });
  if ($('#gesture')) $('#gesture').checked = !!(s.gesture && s.gesture.control);
  if (!$('#v-state')) return;
  const v = s.voice || {}, x = s.xiaozhi || {};
  const state = x.state === 'speaking' ? 'speaking' : ['connecting', 'listening'].includes(x.state) ? 'xiaozhi'
    : v.awake_s > 0 ? 'listening' : has('voice') && !v.on ? 'off' : 'waiting';
  $('#v-state').className = 'state grow ' + state;
  $('#v-state').textContent = t('voice.' + state, {s: Math.ceil(v.awake_s || 0)});
  if ($('#v-on')) $('#v-on').checked = !!v.on;
  $('#v-off').hidden = !has('voice') || !!v.on;
  if ($('#v-stop')) $('#v-stop').hidden = x.state !== 'speaking';
  $('#v-log').innerHTML = remember(s);
}

export function mount(root) {
  el = document.createElement('div');
  el.className = 'grid2';
  el.innerHTML = html();
  root.append(el);
  apply(el);
  stickLabels();
  resets = [joystick($('#stick-l'), (x, y) => { sticks.l = [x, y]; $('#stick-l-v').textContent = `${y.toFixed(2)} / ${x.toFixed(2)}`; }),
    joystick($('#stick-r'), (x, y) => { sticks.r = [x, y]; $('#stick-r-v').textContent = (headMode ? y : x).toFixed(2); })];
  $('#stick-mode').onclick = () => { stop(); headMode = !headMode; lastHead = ''; stickLabels(); };
  $('#cam-btn').onclick = toggleCamera;
  if ($('#gesture')) $('#gesture').onchange = e => act(rpc('gesture.control', {on: e.target.checked}));
  el.querySelectorAll('[data-mode]').forEach(b => b.onclick = () =>
    act(b.dataset.mode === 'off' ? rpc('robot.estop') : rpc('robot.set_mode', {mode: b.dataset.mode})));
  el.querySelector('.skills').onclick = e => {
    const b = e.target.closest('[data-skill]');
    if (b) act(rpc('robot.skill', {name: b.dataset.skill}));
  };
  $('#mouth').oninput = e => {
    $('#mouth-v').textContent = e.target.value + '°';
    act(rpc('robot.mouth', {deg: +e.target.value}));
  };
  const sliders = [...el.querySelectorAll('[data-head]')];
  sliders.forEach((r, i) => r.oninput = () => {
    $('#head-v' + i).textContent = (+r.value).toFixed(2);
    act(rpc('robot.head', {head: sliders.map(s => +s.value)}));
  });
  $('#head-center').onclick = () => act(rpc('robot.head', {head: [0, 0, 0, 0]}));
  if ($('#v-wake')) {
    $('#v-wake').onclick = () => act(rpc('voice.wake'));
    if ($('#v-stop')) $('#v-stop').onclick = () => act(rpc('voice.wake'));
    if ($('#v-on')) $('#v-on').onchange = e => act(rpc('voice.enable', {on: e.target.checked}));
    rpc('sound.list').then(r => {
      $('#quacks').innerHTML = r.sounds.filter(q => q.name !== 'tick')
        .map(q => `<button data-quack="${esc(q.name)}">🦆 ${esc(tr('sound.' + q.name, q.name))}</button>`).join('');
    }).catch(() => {});
    $('#quacks').onclick = e => {
      const b = e.target.closest('[data-quack]');
      if (b) act(rpc('robot.sound', {name: b.dataset.quack}));
    };
  }
  unsub = subscribe(render);
  timer = setInterval(send, 50);
  window.addEventListener('keydown', keydown);
  window.addEventListener('keyup', keyup);
  window.addEventListener('blur', stop);
}

function keydown(e) {
  if (['INPUT', 'SELECT', 'TEXTAREA'].includes(e.target.tagName)) return;
  const k = e.key.toLowerCase();
  if (k.length === 1 && 'wasdqe'.includes(k)) keys.add(k);
}

function keyup(e) {
  keys.delete(e.key.toLowerCase());
}

export function unmount() {
  stop();
  if (moving) act(rpc('robot.stop'));
  moving = false;
  if (cam) toggleCamera();
  clearInterval(timer);
  unsub();
  window.removeEventListener('keydown', keydown);
  window.removeEventListener('keyup', keyup);
  window.removeEventListener('blur', stop);
}
