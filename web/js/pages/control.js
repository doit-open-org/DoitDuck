// Control: an overview (attitude, what the duck is doing, the loops and battery), the camera, the voice bar (voice, xiaozhi and gestures as one log of what was heard
// and done, quack buttons), modes, skills, and a gamepad (sticks and keys). Motion is sent only
// while a stick or key is held, so an idle page holds no control.
import {rpc} from '../rpc.js';
import {store, subscribe, has} from '../store.js';
import {t, tr, L, esc, apply, resultText} from '../i18n.js';
import {toast, act} from '../components/toast.js';
import {joystick} from '../components/joystick.js';
import {attitudeHtml, showAttitude} from '../components/attitude.js';

let el, unsub, timer, resets = [], cam = null;
// A phone on its side (as in layout.css): the camera between the gamepad's two halves, a button there while it is off.
const sideways = window.matchMedia('(orientation: landscape) and (max-height: 520px)');
const sticks = {l: [0, 0], r: [0, 0]}, keys = new Set();
let moving = false, busy = false, lastHead = '0,0,0,0';
let log = [], seen = {};

const $ = s => el.querySelector(s);
const dz = v => Math.abs(v) < 0.1 ? 0 : v;

// The sticks card: the gamepad's keys where they are on a ShanWan Q34B (Xbox layout), and in its bottom corners
// what they do, from robots/<name>.json "gamepad" (pad.status): the left stick and the left-hand and middle keys on
// the left, the right stick and the right-hand keys on the right, each key's combinations after it. Keys doing
// nothing are grey, the emergency stop red. ⌂, O and T are drawn only: the pad keeps them to itself. ls and rs are
// the sticks.
const GLYPH = {lt: 'LT', lb: 'LB', rt: 'RT', rb: 'RB', up: '▲', down: '▼', left: '◀', right: '▶',
  select: '<svg viewBox="0 0 12 12"><rect x="1" y="1" width="7" height="7" rx="1.5"/><rect x="4" y="4" width="7" height="7" rx="1.5"/></svg>',
  start: '≡', x: 'X', y: 'Y', a: 'A', b: 'B', ls: 'L', rs: 'R'};
const PAD_L = ['lt', 'lb', 'ls', 'start', 'up', 'down', 'left', 'right', 'select'], PAD_R = ['rt', 'rb', 'y', 'x', 'b', 'a', 'rs'];
let padButtons = {};                         /* pad.status: "x" and "lt+x" bindings (features/gamepad) */
const heldKeys = new Set(), held = {};      /* the page's keys held down; the action each key's press started */
// The buttons some binding holds first: held only, running nothing of their own.
const mods = () => new Set(Object.keys(padButtons).filter(n => n.includes('+')).map(n => n.split('+')[0]));
// Key id's own action; the sticks walk and look by default.
const plain = id => padButtons[id] || {ls: 'walk', rs: 'look'}[id];
// Key id's action with the page's keys held: a binding with one of them held first, else its own.
function bound(id) {
  for (const m of heldKeys) if (padButtons[`${m}+${id}`]) return padButtons[`${m}+${id}`];
  return plain(id);
}
const key = id => `<i class="pk" data-key="${id}">${GLYPH[id]}</i>`;

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
      ${has('xiaozhi') ? '<button id="v-stop" data-i18n="control.interrupt" hidden></button><button id="v-hang" class="danger" data-i18n="control.hang_up" hidden></button>' : ''}</div>
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
  <section class="card pad-card">
    <h2 class="landscape-hide" data-i18n="control.sticks"></h2>
    <div class="pad">${key('lt')}${key('lb')}${key('rt')}${key('rb')}
      <div class="pad-mid"><div>${key('select')}<i class="pk off">⌂</i>${key('start')}</div>
        <div><i class="pk off"><svg viewBox="0 0 12 12"><circle cx="6" cy="6" r="4"/></svg></i><i class="pk off">T</i></div></div>
      <div id="stick-l"></div><div class="abxy">${key('y')}${key('x')}${key('b')}${key('a')}</div>
      <div class="dpad">${key('up')}${key('left')}${key('right')}${key('down')}</div><div id="stick-r"></div>
      <ul id="pad-l" class="pad-legend"></ul><ul id="pad-r" class="pad-legend"></ul><div class="pad-cam"><button id="pad-cam-btn" data-i18n="control.camera_on"></button></div></div>
    <p class="muted landscape-hide" data-i18n="control.pad_hint"></p>
    <p class="muted landscape-hide keys-hint" data-i18n="control.keys"></p>
  </section></div>`;
}

// What each key does, alone and with another held first.
function padKeys() {
  const fn = a => {
    if (!a.startsWith('skill:')) return tr(`pad.fn_${a}`, a);
    const s = store.info.skills.find(k => k.name === a.slice(6));
    return s ? L(s.label) : a.slice(6);
  };
  const m = mods();
  const combos = id => Object.entries(padButtons).filter(([n]) => n.split('+')[1] === id);
  const li = (badge, s) => `<li><i class="pk">${badge}</i><span>${esc(s)}</span></li>`;
  const legend = ids => ids.map(id => (m.has(id) ? li(GLYPH[id], t('pad.mod')) : plain(id) ? li(GLYPH[id], fn(plain(id))) : '')
    + combos(id).map(([n, a]) => li(`${GLYPH[n.split('+')[0]]}+${GLYPH[id]}`, fn(a))).join('')).join('');
  $('#pad-l').innerHTML = legend(PAD_L);
  $('#pad-r').innerHTML = legend(PAD_R);
  el.querySelectorAll('.pad [data-key]').forEach(k => {
    const id = k.dataset.key;
    k.classList.toggle('off', !m.has(id) && !plain(id) && !combos(id).length);
    k.classList.toggle('stop', padButtons[k.dataset.key] === 'estop');
  });
}

// A key of the page's gamepad went down or up: what the gamepad's would do (features/gamepad), from the page.
// A key some binding holds first is only held; a key let go ends what its press started.
function press(id, down) {
  if (mods().has(id)) { if (down) heldKeys.add(id); else heldKeys.delete(id); return; }
  const a = down ? bound(id) : held[id];
  held[id] = down ? a : undefined;
  if (a === 'mouth_hold') act(rpc('robot.mouth', {deg: down ? store.info.mouth_max : 0}));
  else if (!down || !a) return;
  else if (a === 'estop') act(rpc('robot.estop'));
  else if (a === 'start_cycle') act(rpc('robot.set_mode', {mode: store.status && store.status.mode === 'hold' ? 'walk' : 'hold'}));
  else if (a.startsWith('skill:')) act(rpc('robot.skill', {name: a.slice(6)}));
}

// Every 50 ms, as the gamepad: each stick does its action (walk, walk_side, look, neck_tilt) and the W A S D Q E
// keys walk; the body's twist while something is held, the head when it changes.
function send() {
  if (document.hidden || busy) return;
  const tw = [0, 0, 0], h = [0, 0, 0, 0];
  [['ls', sticks.l], ['rs', sticks.r]].forEach(([id, [sx, sy]]) => {
    const x = dz(sx), y = dz(sy), a = bound(id);
    if (a === 'walk') { tw[0] += y; tw[2] -= x; }
    else if (a === 'walk_side') { tw[0] += y; tw[1] -= x; }
    else if (a === 'look') { h[1] -= y; h[2] -= x; }
    else if (a === 'neck_tilt') { h[0] -= y; h[3] += x; }
  });
  [['w', 's'], ['a', 'd'], ['q', 'e']].forEach(([a, b], i) => {
    if (keys.has(a) || keys.has(b)) tw[i] = (keys.has(a) - keys.has(b)) * 0.5;
  });
  tw.forEach((v, i) => tw[i] = v * store.info.vmax[i]);
  const head = h.map(v => +v.toFixed(2));
  const active = tw.some(v => v !== 0), calls = [];
  if (active || moving) calls.push(rpc('robot.move', {vx: tw[0], vy: tw[1], wz: tw[2]}).catch(e => { toast(e); stop(); }));
  moving = active;
  if (head.join() !== lastHead) calls.push(rpc('robot.head', {head}).catch(toast));
  lastHead = head.join();
  if (!calls.length) return;
  busy = true;
  Promise.all(calls).finally(() => busy = false);
}

function stop() {
  keys.clear();
  resets.forEach(r => r());
  el.querySelectorAll('.pad .pk.down').forEach(k => { k.classList.remove('down'); press(k.dataset.key, false); });
}

function toggleCamera() {
  if (cam) {
    cam.ws.close();
    cam.mux.destroy();
    cam = null;
    $('#cam').hidden = true;
    $('#cam-btn').textContent = t('control.camera_on');
    $('#pad-cam-btn').hidden = false;
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
  $('#pad-cam-btn').hidden = true;
}

// The camera's picture in the gamepad when sideways, in its card otherwise; a moved video pauses, so it plays again.
function placeCamera() {
  const v = $('#cam');
  if (sideways.matches) {
    $('.pad-cam').append(v);
  } else {
    $('#cam-btn').closest('section').append(v);
  }
  v.play().catch(() => {});
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
  if ($('#v-hang')) $('#v-hang').hidden = !['connecting', 'listening', 'speaking'].includes(x.state);
  $('#v-log').innerHTML = remember(s);
}

export function mount(root) {
  el = document.createElement('div');
  el.className = 'grid2 control';
  el.innerHTML = html();
  root.append(el);
  apply(el);
  padKeys();
  if (has('gamepad')) rpc('pad.status').then(p => { padButtons = p.buttons; padKeys(); }).catch(() => {});
  resets = [joystick($('#stick-l'), (x, y) => { sticks.l = [x, y]; }), joystick($('#stick-r'), (x, y) => { sticks.r = [x, y]; })];
  // The keys press while held, so a key can be held with one finger and a stick or key pressed with another.
  el.querySelectorAll('.pad [data-key]').forEach(k => {
    if (k.closest('.pad-legend')) return;
    k.onpointerdown = e => { if (k.classList.contains('off')) return; k.setPointerCapture(e.pointerId); k.classList.add('down'); press(k.dataset.key, true); };
    k.onpointerup = k.onpointercancel = () => { if (k.classList.contains('down')) { k.classList.remove('down'); press(k.dataset.key, false); } };
  });
  $('#cam-btn').onclick = $('#pad-cam-btn').onclick = toggleCamera;
  if ($('#gesture')) $('#gesture').onchange = e => act(rpc('gesture.control', {on: e.target.checked}));
  el.querySelectorAll('[data-mode]').forEach(b => b.onclick = () =>
    act(b.dataset.mode === 'off' ? rpc('robot.estop') : rpc('robot.set_mode', {mode: b.dataset.mode})));
  el.querySelector('.skills').onclick = e => {
    const b = e.target.closest('[data-skill]');
    if (b) act(rpc('robot.skill', {name: b.dataset.skill}));
  };
  if ($('#v-wake')) {
    $('#v-wake').onclick = () => act(rpc('voice.wake'));
    if ($('#v-stop')) $('#v-stop').onclick = () => act(rpc('voice.wake'));
    if ($('#v-hang')) $('#v-hang').onclick = () => act(rpc('xiaozhi.hangup'));
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
  sideways.addEventListener('change', placeCamera);
  if (sideways.matches) placeCamera();
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
  sideways.removeEventListener('change', placeCamera);
}
