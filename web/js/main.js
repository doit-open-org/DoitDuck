// Start-up, the top bar, the first-use checks and the pages (#control, #actions, ...).
import {connect, connected, onConn, rpc} from './rpc.js';
import {store, subscribe} from './store.js';
import {initI18n, apply, t, esc, getLang, setLang} from './i18n.js';
import {toast, act} from './components/toast.js';
import * as control from './pages/control.js';
import * as actions from './pages/actions.js';
import * as status from './pages/status.js';
import * as maintain from './pages/maintain.js';
import * as settings from './pages/settings.js';

const PAGES = {
  control: [control, 'M4 12h4M6 10v4M15 11h.01M18 13h.01M7 6h10a5 5 0 0 1 5 5v2a4 4 0 0 1-7 2.5L13 14h-2l-2 1.5A4 4 0 0 1 2 13v-2a5 5 0 0 1 5-5z'],
  actions: [actions, 'M4 4h16v6H4zM4 14h7v6H4zM15 14h5v6h-5z'],
  status: [status, 'M3 12h4l3-8 4 16 3-8h4'],
  maintain: [maintain, 'M14 7l3-3 3 3-3 3M17 4l-9 9M4 20l5-5M8 13l3 3'],
  settings: [settings, 'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2'],
};
const $ = s => document.querySelector(s);
let current = null, currentId = '';

function nav() {
  $('#nav').innerHTML = Object.entries(PAGES).map(([id, [, d]]) =>
    `<a href="#${id}" data-page="${id}"><svg viewBox="0 0 24 24"><path d="${d}"/></svg><span>${esc(t('nav.' + id))}</span></a>`).join('');
}

function route() {
  const id = PAGES[location.hash.slice(1)] ? location.hash.slice(1) : 'control';
  if (current) current.unmount();
  currentId = id;
  current = PAGES[id][0];
  document.querySelectorAll('#nav a').forEach(a => a.classList.toggle('on', a.dataset.page === id));
  const view = $('#view');
  view.innerHTML = '';
  if (store.info) current.mount(view);
  checkFirstUse();
}

// What a new duck still needs: walk and get-up policies, the zero positions, the torso level.
async function checkFirstUse() {
  if (!store.info) return;
  try {
    const [pl, cal] = await Promise.all([rpc('policy.list'), rpc('calib.export')]);
    const slots = pl.policies.map(p => p.slot);
    const steps = cal.calibration.steps;
    const todo = [];
    if (!slots.includes('walk') || !slots.includes('stand')) todo.push(['#actions', 'first.policies']);
    if (!steps.zero) todo.push(['#maintain', 'first.calibrate']);
    if (!steps.level) todo.push(['#maintain', 'first.level']);
    // Closed stays closed in this browser until the list changes.
    const key = todo.map(x => x[1]).join();
    $('#banner').hidden = !todo.length || localStorage.getItem('banner-closed') === key;
    $('#banner').innerHTML = todo.map(([h, k]) => `<a href="${h}">${esc(t(k))}</a>`).join('')
      + `<button class="close" title="${esc(t('top.close'))}">×</button>`;
    $('#banner .close').onclick = () => { localStorage.setItem('banner-closed', key); $('#banner').hidden = true; };
  } catch (e) { /* checked again on the next page */ }
}

function top(s) {
  const owner = s.owner !== 'none' ? t('owner.' + s.owner) : '';
  $('#mode').textContent = t('mode.' + s.mode) + (s.enabled ? ' · ' + t('top.torque') : '');
  $('#mode').classList.toggle('armed', s.enabled);
  $('#owner').hidden = !owner;
  $('#owner').textContent = owner;
  $('#battery').hidden = !s.battery_v;
  $('#battery').textContent = s.battery_v.toFixed(2) + ' V';
  $('#battery').classList.toggle('bad', s.battery_v > 0 && s.battery_v < 6.8);
}

async function loadInfo() {
  try {
    store.info = await rpc('robot.info');
    route();
  } catch (e) { setTimeout(loadInfo, 2000); }
}

async function start() {
  await initI18n();
  apply();
  nav();
  $('#estop').onclick = () => act(rpc('robot.estop'));
  // Space is the emergency stop everywhere but in a text field.
  window.addEventListener('keydown', e => {
    if (e.code === 'Space' && !['INPUT', 'SELECT', 'TEXTAREA'].includes(e.target.tagName)) { e.preventDefault(); $('#estop').click(); }
  });
  window.addEventListener('hashchange', route);
  // The language button in the corner names the other language; a new language also relabels the connection.
  const langLabel = () => {
    $('#lang').textContent = getLang() === 'zh' ? 'EN' : '中';
    $('#conn span').textContent = t(connected() ? 'top.online' : 'top.offline');
  };
  langLabel();
  $('#lang').onclick = () => setLang(getLang() === 'zh' ? 'en' : 'zh');
  window.addEventListener('langchange', () => { langLabel(); nav(); route(); });
  onConn(on => {
    $('#conn').classList.toggle('on', on);
    $('#conn span').textContent = t(on ? 'top.online' : 'top.offline');
    if (on && !store.info) loadInfo();
    if (!on) toast(t('top.offline'));
  });
  subscribe(top);
  connect();
}

start();

