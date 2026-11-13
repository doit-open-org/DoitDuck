// Settings: Wi-Fi (so a new board reached over USB gets on a network), sounds
// (volume, idle quacks, the user's recordings), and what voice,
// xiaozhi, gestures and the gamepad are configured with. Voice, gesture and gamepad mappings
// live in robots/<name>.json and are shown here as they are.
import {rpc, post, onConn} from '../rpc.js';
import {store, has} from '../store.js';
import {t, tr, esc, apply} from '../i18n.js';
import {toast, act} from '../components/toast.js';
import {base64} from '../onnx.js';

let el, timer;
const $ = s => el.querySelector(s);
const kv = rows => `<dl class="kv">${rows.map(([k, v]) => `<dt>${esc(t(k))}</dt><dd>${esc(v)}</dd>`).join('')}</dl>`;
const call = c => `${c.call}${c.params ? ' ' + JSON.stringify(c.params) : ''}`;

// An upgrade package of this kind (app or models, tools/pack_ota.sh): its version.json is read here first, to
// show what changes, then the package goes in 1 MB pieces, base64, one POST each (the device takes 4 MB at most);
// the last one installs it. Once the program has started again and the page is back on line, it loads again.
async function upgrade(kind) {
  const file = $(`#ota-${kind}-file`).files[0];
  if (!file) return toast(t('settings.ota_need'));
  const buttons = el.querySelectorAll('[data-ota]');
  try {
    // The .tar: 512-byte headers, the name at 0 and the size (octal) at 124, each followed by its data.
    const tar = new Uint8Array(await new Response(file.stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    const text = b => new TextDecoder().decode(b).replace(/\0[^]*$/, '');
    let m = null;
    for (let off = 0; off + 512 <= tar.length && tar[off];) {
      const size = parseInt(text(tar.subarray(off + 124, off + 136)), 8) || 0;
      if (text(tar.subarray(off, off + 100)).replace(/^\.\//, '') === 'version.json') m = JSON.parse(text(tar.subarray(off + 512, off + 512 + size)));
      off += 512 + Math.ceil(size / 512) * 512;
    }
    if (!m || m.kind !== kind) return toast(t('settings.ota_wrong'));
    const now = await rpc('system.versions');
    const changes = kind === 'app' ? `${now.app} → ${m.version}`
      : Object.entries(m.models).map(([f, v]) => `${f}: ${(now.models.find(x => x.file === f) || {}).version || '—'} → ${v}`).join('\n');
    if (!window.confirm(t('settings.ota_confirm', {changes}))) return;
    buttons.forEach(b => { b.disabled = true; });
    const bytes = new Uint8Array(await file.arrayBuffer()), CHUNK = 1 << 20;
    for (let off = 0; off < bytes.length; off += CHUNK) {
      const last = off + CHUNK >= bytes.length;
      $('#ota-state').textContent = last ? t('settings.ota_installing') : t('settings.ota_sending', {pct: Math.round(off * 100 / bytes.length)});
      await post('system.ota', {kind, offset: off, data: base64(bytes.subarray(off, off + CHUNK)), last});
    }
    $('#ota-state').textContent = t('settings.ota_done');
    onConn(on => { if (on) location.reload(); });
  } catch (e) {
    toast(e);
    buttons.forEach(b => { b.disabled = false; });
    $('#ota-state').textContent = '';
  }
}

// A recording as a 16 kHz mono 16-bit WAV of at most 3 s, what sound.upload takes.
async function toWav(file) {
  const ctx = new AudioContext();
  const src = await ctx.decodeAudioData(await file.arrayBuffer());
  ctx.close();
  const off = new OfflineAudioContext(1, Math.ceil(Math.min(src.duration, 3) * 16000), 16000);
  const node = off.createBufferSource();
  node.buffer = src;
  node.connect(off.destination);
  node.start();
  const pcm = (await off.startRendering()).getChannelData(0);
  const v = new DataView(new ArrayBuffer(44 + pcm.length * 2));
  const str = (o, s) => [...s].forEach((c, i) => v.setUint8(o + i, c.charCodeAt(0)));
  str(0, 'RIFF'); v.setUint32(4, 36 + pcm.length * 2, true); str(8, 'WAVEfmt ');
  v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, 16000, true); v.setUint32(28, 32000, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  str(36, 'data'); v.setUint32(40, pcm.length * 2, true);
  pcm.forEach((x, i) => v.setInt16(44 + i * 2, Math.max(-1, Math.min(1, x)) * 32767, true));
  return new Uint8Array(v.buffer);
}

async function sounds() {
  const r = await rpc('sound.list');
  $('#volume').value = r.volume_db;
  $('#volume-v').textContent = r.volume_db + ' dB';
  $('#idle').checked = r.idle;
  $('#sounds').innerHTML = r.sounds.map(q => `<tr><td>${esc(tr('sound.' + q.name, q.name))} <small>${esc(q.name)}</small></td>
    <td class="num">${q.seconds.toFixed(2)} s</td><td><button data-play="${esc(q.name)}">▶</button>
    ${q.source === 'user' ? `<button data-remove="${esc(q.name)}">${esc(t(q.builtin ? 'settings.restore' : 'actions.remove'))}</button>` : ''}</td></tr>`).join('');
}

// Wi-Fi: the network in use and its address, or how far connecting has got; and the hotspot, when it is open.
let wifiAp = false, wifiAlways = false;
async function wifiState() {
  const w = await rpc('wifi.status');
  const on = w.state === 'COMPLETED';
  wifiAp = w.ap;
  wifiAlways = w.ap_always;
  $('#wifi-ap').textContent = t(w.ap ? 'settings.wifi_ap_stop' : 'settings.wifi_ap');
  $('#wifi-always').checked = w.ap_always;
  $('#wifi-now').innerHTML = kv([['settings.wifi_state', on ? t('settings.wifi_on') : w.state === 'off' ? t('settings.wifi_off') : t('settings.wifi_joining')],
    ['settings.wifi_net', w.ssid || '—'], ['settings.wifi_ip', w.ip || '—'],
    ...(w.ap ? [['settings.wifi_hotspot', t('settings.wifi_ap_on', {ssid: w.ap_ssid})]] : [])])
    + (on && w.ip && w.ip !== location.hostname ? `<p>${esc(t('settings.wifi_open'))} <a href="http://${esc(w.ip)}/">http://${esc(w.ip)}/</a></p>` : '');
  return w;
}

// After Connect: the state every second until the network is joined and has its address (shown in a dialog), or 45 s pass.
async function wifiJoin() {
  $('#wifi-go').disabled = true;
  try {
    for (const end = Date.now() + 45000; Date.now() < end && el.isConnected;) {
      await new Promise(done => setTimeout(done, 1000));
      const w = await wifiState();
      if (w.state === 'COMPLETED' && w.ip) {
        $('#wifi-done-text').textContent = t('settings.wifi_joined', {ssid: w.ssid, ip: w.ip});
        $('#wifi-done-ok').textContent = t(wifiAp && !wifiAlways ? 'settings.wifi_joined_close' : 'settings.wifi_joined_ok');
        $('#wifi-done').showModal();
        return;
      }
    }
    if (el.isConnected) toast(t('settings.wifi_failed'));
  } finally {
    $('#wifi-go').disabled = false;
  }
}

async function wifiScan() {
  // A scan takes a few seconds: what is known now, then again once it is done.
  for (const wait of [0, 3500]) {
    await new Promise(ok => setTimeout(ok, wait));
    const r = await rpc('wifi.scan');
    $('#wifi-list').innerHTML = r.networks.map(n => `<button data-ssid="${esc(n.ssid)}">${n.secure ? '🔒' : ''} ${esc(n.ssid)}
      <small>${n.signal} dBm</small></button>`).join('') || `<span class="muted">${esc(t('settings.wifi_none'))}</span>`;
  }
}

// Live parts: Wi-Fi, voice and xiaozhi state, gamepad connection.
async function refresh() {
  if (document.hidden) return;
  try {
    if (has('wifi')) await wifiState();
    if (has('xiaozhi')) {
      const x = await rpc('xiaozhi.status');
      $('#xz').innerHTML = kv([['settings.xz_state', t('xz.' + x.state)], ['settings.xz_reason', x.reason ? tr('xzr.' + x.reason, x.reason) : '—'],
        ['settings.xz_code', x.activation_code || '—'], ['settings.xz_device', x.device_id], ['settings.xz_ota', x.ota_url]]);
    }
    if (has('voice')) {
      const v = await rpc('voice.status');
      $('#voice-state').innerHTML = kv([['settings.voice_engine', t(v.engine ? 'settings.yes' : 'settings.no')],
        ['settings.voice_mic', t(v.audio ? 'settings.yes' : 'settings.no')], ['settings.voice_wake', v.wake_words.join(', ')]]);
    }
    if (has('gamepad')) {
      const p = await rpc('pad.status');
      $('#pad').innerHTML = kv([['settings.pad_state', t(p.connected ? 'settings.yes' : 'settings.no')]].concat(
        Object.entries(p.buttons).map(([k, v]) => [k.toUpperCase(), v])));
    }
  } catch (e) { /* shown again next time */ }
}

export function mount(root) {
  const info = store.info;
  el = document.createElement('div');
  el.className = 'grid2';
  // Two columns on a wide screen: this page's own settings, and what the features are configured with.
  el.innerHTML = `<div class="col">
  ${has('wifi') ? `<section class="card"><h2 data-i18n="settings.wifi"></h2><div id="wifi-now"></div>
    <div class="row"><button id="wifi-scan" data-i18n="settings.wifi_scan"></button><button id="wifi-ap" data-i18n="settings.wifi_ap"></button></div>
    <label class="row"><input id="wifi-always" type="checkbox"><span data-i18n="settings.wifi_ap_always"></span></label><div id="wifi-list" class="nets"></div>
    <input id="wifi-ssid" type="text" placeholder="${esc(t('settings.wifi_ssid'))}">
    <input id="wifi-pw" type="password" placeholder="${esc(t('settings.wifi_pw'))}">
    <div class="row"><button id="wifi-go" class="primary" data-i18n="settings.wifi_connect"></button></div>
    <p class="muted" data-i18n="settings.wifi_help"></p>
    <dialog id="wifi-done"><p id="wifi-done-text"></p><div class="row"><button id="wifi-done-ok" class="primary"></button></div></dialog></section>` : ''}
  <section class="card"><h2 data-i18n="settings.sound"></h2>
    <label><span data-i18n="settings.volume"></span> <b id="volume-v" class="num"></b><input id="volume" type="range" min="-40" max="10" step="1"></label>
    <label class="row"><input id="idle" type="checkbox"><span data-i18n="settings.idle"></span></label>
    <table class="compact"><tbody id="sounds"></tbody></table>
    <h3 data-i18n="settings.upload"></h3><p class="muted" data-i18n="settings.upload_help"></p>
    <input id="up-file" type="file" accept="audio/*">
    <div class="row"><input id="up-name" type="text" placeholder="quack" class="grow"><button id="up" class="primary" data-i18n="actions.upload"></button></div></section>
  <section class="card"><h2 data-i18n="settings.smoothing"></h2><p class="muted" data-i18n="settings.smoothing_help"></p>
    <label><span data-i18n="settings.smooth_leg"></span> <b id="leg-v" class="num"></b><input id="leg" type="range" min="0" max="1" step="0.05"></label>
    <label><span data-i18n="settings.smooth_head"></span> <b id="head-v" class="num"></b><input id="head" type="range" min="0" max="1" step="0.05"></label></section>
  </div><div class="col">
  ${has('voice') ? `<section class="card"><h2 data-i18n="settings.voice"></h2><div id="voice-state"></div>
    <details><summary><span data-i18n="settings.commands"></span> <span id="n-commands" class="muted"></span></summary>
    <table><tbody id="commands"></tbody></table><p class="muted" data-i18n="settings.in_config"></p></details></section>` : ''}
  ${has('xiaozhi') ? `<section class="card"><h2 data-i18n="settings.xiaozhi"></h2><div id="xz"></div><p class="muted" data-i18n="settings.xz_help"></p></section>` : ''}
  ${has('gesture') ? `<section class="card"><h2 data-i18n="settings.gesture"></h2><table><tbody id="gestures"></tbody></table><p class="muted" data-i18n="settings.in_config"></p></section>` : ''}
  ${has('gamepad') ? `<section class="card"><h2 data-i18n="settings.gamepad"></h2><div id="pad"></div></section>` : ''}
  <section class="card"><h2 data-i18n="settings.about"></h2>
    ${kv([['settings.version', info.version], ['settings.board', info.board], ['settings.robot', info.name], ['settings.features', info.features.join(', ')]])}
    <p data-i18n="settings.licenses"></p></section>
  <section class="card"><h2 data-i18n="settings.ota"></h2><p class="muted" data-i18n="settings.ota_help"></p>
    <h3 data-i18n="settings.ota_app"></h3>${kv([['settings.version', info.version]])}
    <input id="ota-app-file" type="file" accept=".gz,.tgz">
    <div class="row"><button data-ota="app" class="primary" data-i18n="settings.ota_app_go"></button></div>
    <h3 data-i18n="settings.ota_models"></h3><table class="compact"><tbody id="ota-models"></tbody></table>
    <input id="ota-models-file" type="file" accept=".gz,.tgz">
    <div class="row"><button data-ota="models" class="primary" data-i18n="settings.ota_models_go"></button></div>
    <p id="ota-state" class="muted"></p></section></div>`;
  root.append(el);
  apply(el);
  if (has('wifi')) {
    $('#wifi-scan').onclick = () => wifiScan().catch(toast);
    $('#wifi-list').onclick = e => {
      const b = e.target.closest('[data-ssid]');
      if (b) { $('#wifi-ssid').value = b.dataset.ssid; $('#wifi-pw').focus(); }
    };
    $('#wifi-go').onclick = () => act(rpc('wifi.connect', {ssid: $('#wifi-ssid').value, password: $('#wifi-pw').value})
      .then(() => { toast(t('settings.wifi_sent')); $('#wifi-pw').value = ''; return wifiJoin(); }));
    $('#wifi-done-ok').onclick = () => {
      $('#wifi-done').close();
      if (wifiAp && !wifiAlways) act(rpc('wifi.ap', {on: false}).then(wifiState));
    };
    $('#wifi-ap').onclick = () => act(rpc('wifi.ap', {on: !wifiAp}).then(wifiState));
    $('#wifi-always').onchange = e => act(rpc('wifi.ap', {always: e.target.checked}).then(wifiState));
  }
  rpc('system.versions').then(v => {
    $('#ota-models').innerHTML = v.models.sort((a, b) => a.file.localeCompare(b.file))
      .map(m => `<tr><td>${esc(m.file)}</td><td class="num">${esc(m.version || '—')}</td></tr>`).join('');
  }).catch(toast);
  el.querySelectorAll('[data-ota]').forEach(b => { b.onclick = () => upgrade(b.dataset.ota); });
  $('#volume').oninput = e => { $('#volume-v').textContent = e.target.value + ' dB'; };
  $('#volume').onchange = e => act(rpc('sound.settings', {volume_db: +e.target.value}));
  $('#idle').onchange = e => act(rpc('sound.settings', {idle: e.target.checked}));
  $('#sounds').onclick = e => {
    const b = e.target.closest('button');
    if (b && b.dataset.play) act(rpc('robot.sound', {name: b.dataset.play}));
    if (b && b.dataset.remove) act(rpc('sound.remove', {name: b.dataset.remove}).then(sounds));
  };
  $('#up').onclick = async () => {
    const file = $('#up-file').files[0], name = $('#up-name').value.trim();
    if (!file || !name) return toast(t('settings.upload_need'));
    try {
      await post('sound.upload', {name, wav: base64(await toWav(file))});
      toast(t('settings.uploaded'));
      sounds();
    } catch (e) { toast(e); }
  };
  // robot.filter with no params reads the smoothing; with them it sets it.
  const smooth = (p = {}) => rpc('robot.filter', p).then(r => {
    $('#leg').value = r.leg; $('#leg-v').textContent = r.leg.toFixed(2);
    $('#head').value = r.head; $('#head-v').textContent = r.head.toFixed(2);
  }).catch(toast);
  $('#leg').onchange = $('#head').onchange = () => smooth({leg: +$('#leg').value, head: +$('#head').value});
  smooth();
  rpc('config.get').then(c => {
    const r = c.robot;
    if ($('#n-commands')) $('#n-commands').textContent = `(${(r.voice && r.voice.commands || []).length})`;
    if ($('#commands')) $('#commands').innerHTML = (r.voice && r.voice.commands || []).map(c => `<tr><td>${esc(c.words)}</td><td><small>${esc(call(c))}</small></td></tr>`).join('');
    if ($('#gestures')) $('#gestures').innerHTML = (r.gesture && r.gesture.commands || [])
      .map(c => `<tr><td>${esc(tr('gesture.' + c.gesture, c.gesture))}</td><td><small>${esc(call(c))}</small></td></tr>`).join('');
  }).catch(toast);
  sounds().catch(toast);
  refresh();
  timer = setInterval(refresh, 2000);
}

export function unmount() {
  clearInterval(timer);
}
