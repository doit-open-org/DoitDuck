// Actions: the policy library, and importing one. The import wizard converts the ONNX here,
// checks it as the robot will, uploads it, runs it in preview with its joint targets drawn,
// and marks it verified when the user is satisfied (docs/doitduck-design.md §6.4).
import {rpc, post} from '../rpc.js';
import {store, subscribe} from '../store.js';
import {t, L, esc, apply} from '../i18n.js';
import {toast, act} from '../components/toast.js';
import {convert, checkManifest, base64} from '../onnx.js';
import {chart} from '../components/chart.js';

let el, unsub = null, compatible = [], trying = '';
const $ = s => el.querySelector(s);
const imp = {weights: null, manifest: null};

async function list() {
  const r = await rpc('policy.list');
  compatible = r.compatible_models || [];
  // Walk and get-up run by the mode, not as a skill.
  const base = p => p.slot === 'walk' || p.slot === 'stand';
  $('#list').innerHTML = r.policies.map(p => `<div class="card">
    <div class="row"><b class="grow">${esc(L(p.label))} <small>${esc(p.name)}</small></b>
      <span class="chip ${p.verified ? 'ok' : 'warn'}">${esc(t(p.verified ? 'actions.verified' : 'actions.unverified'))}</span></div>
    <p class="muted">${esc(t('kind.' + (base(p) ? p.slot : p.kind)))} · ${esc(t('source.' + p.source))}${p.in_use ? ' · ' + esc(t('actions.in_use')) : ''}</p>
    ${p.description ? `<p>${esc(p.description)}</p>` : ''}
    <div class="row">
      ${base(p) ? '' : `<button data-try="${esc(p.name)}">${esc(t('actions.try'))}</button>`}
      ${p.verified ? '' : `<button data-verify="${esc(p.name)}" class="primary">${esc(t('actions.verify'))}</button>`}
      ${p.source === 'imported' ? `<button data-remove="${esc(p.name)}">${esc(t('actions.remove'))}</button>` : ''}
    </div></div>`).join('');
}

function showChecks(checks) {
  $('#checks').innerHTML = checks.map(([k, ok, d]) => `<li class="${ok ? 'ok' : 'bad'}">${esc(t('check.' + k))} <small>${esc(d)}</small></li>`).join('');
  $('#upload').disabled = !(imp.weights && imp.manifest && checks.every(c => c[1]));
}

async function readFiles() {
  const onnx = $('#f-onnx').files[0], man = $('#f-manifest').files[0];
  imp.weights = imp.manifest = null;
  const checks = [];
  try {
    if (onnx) {
      const r = convert(await onnx.arrayBuffer());
      imp.weights = r.weights;
      checks.push(...r.checks);
    }
    if (man) {
      imp.manifest = JSON.parse(await man.text());
      if (imp.manifest.policies) throw new Error(t('actions.one_policy'));
      $('#name').value = imp.manifest.name || '';
      checks.push(...checkManifest(imp.manifest, compatible));
    }
  } catch (e) {
    checks.push(['file', false, e.message]);
  }
  showChecks(checks);
}

async function upload(confirm = false) {
  try {
    const r = await post('policy.import', {name: $('#name').value.trim(), manifest: imp.manifest, weights: base64(imp.weights), confirm});
    toast(t('actions.imported', {name: r.name}));
    trying = r.name;
    $('#step-try').hidden = false;
    $('#try-name').textContent = r.name;
    list();
  } catch (e) {
    if (e.code === 'policy_replace_base' && window.confirm(t('actions.confirm_base'))) return upload(true);
    toast(e);
  }
}

// Runs a policy in preview (torque off): a skill by robot.skill; its targets are drawn as it runs.
async function tryPolicy(name) {
  trying = name;
  $('#step-try').hidden = false;
  $('#try-name').textContent = name;
  try {
    const s = store.status;
    if (s && s.mode !== 'preview') await rpc('robot.set_mode', {mode: 'preview'});
    await rpc('robot.skill', {name});
  } catch (e) { toast(e); }
}

export function mount(root) {
  el = document.createElement('div');
  el.style.display = 'contents';
  el.innerHTML = `
  <section class="card">
    <h2 data-i18n="actions.import"></h2>
    <p class="muted" data-i18n="actions.import_help"></p>
    <div class="grid2">
      <label><span data-i18n="actions.onnx"></span><br><input id="f-onnx" type="file" accept=".onnx"></label>
      <label><span data-i18n="actions.manifest"></span><br><input id="f-manifest" type="file" accept=".json,application/json"></label>
    </div>
    <ul id="checks" class="check"></ul>
    <div class="row"><input id="name" type="text" class="grow" placeholder="name"><button id="upload" class="primary" disabled data-i18n="actions.upload"></button></div>
    <div id="step-try" class="stack" hidden>
      <h3><span data-i18n="actions.try_title"></span> <span id="try-name"></span></h3>
      <p class="muted" data-i18n="actions.try_help"></p>
      <canvas id="chart" class="chart"></canvas>
      <div class="row"><button id="try-run" data-i18n="actions.try"></button><button id="try-ok" class="primary" data-i18n="actions.verify"></button></div>
    </div>
  </section>
  <div id="list" class="grid2"></div>`;
  root.append(el);
  apply(el);
  $('#f-onnx').onchange = $('#f-manifest').onchange = readFiles;
  $('#upload').onclick = () => upload();
  $('#try-run').onclick = () => tryPolicy(trying);
  $('#try-ok').onclick = () => act(rpc('policy.verify', {name: trying}).then(() => { toast(t('actions.verified')); list(); }));
  $('#list').onclick = e => {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.try) tryPolicy(b.dataset.try);
    if (b.dataset.verify) act(rpc('policy.verify', {name: b.dataset.verify}).then(list));
    if (b.dataset.remove && window.confirm(t('actions.confirm_remove', {name: b.dataset.remove})))
      act(rpc('policy.remove', {name: b.dataset.remove}).then(list));
  };
  const draw = chart($('#chart'));
  // Legs only (the first five motors' targets): enough to see whether the policy does something sane.
  unsub = subscribe(s => { if (!$('#step-try').hidden) draw(s.targets.slice(0, 5)); });
  list().catch(toast);
}

export function unmount() {
  unsub();
}
