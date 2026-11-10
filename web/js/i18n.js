// Chinese and English. Fixed text names its key in a data-i18n attribute; script text uses t(key, params).
// A missing key falls back to English, then to the key itself. The language follows the
// browser until chosen in the settings, and is kept in localStorage.

let lang = 'en', dict = {}, en = {};

const load = l => fetch(`i18n/${l}.json`).then(r => r.json());

export async function initI18n() {
  lang = localStorage.getItem('lang') || (navigator.language.startsWith('zh') ? 'zh' : 'en');
  en = await load('en');
  dict = lang === 'en' ? en : await load(lang);
  document.documentElement.lang = lang;
}

export const getLang = () => lang;

export async function setLang(l) {
  localStorage.setItem('lang', l);
  await initI18n();
  apply();
  window.dispatchEvent(new Event('langchange'));
}

export function t(key, params) {
  let s = dict[key] ?? en[key] ?? key;
  if (params) s = s.replace(/\{(\w+)\}/g, (m, k) => params[k] ?? m);
  return s;
}

export function apply(root = document) {
  root.querySelectorAll('[data-i18n]').forEach(e => { e.textContent = t(e.dataset.i18n); });
}

// key's text, or fallback when neither language has it.
export const tr = (key, fallback) => dict[key] ?? en[key] ?? fallback;

// A result the device reports (voice, gestures): done, woken, ... or an error code.
export const resultText = code => tr('result.' + code, tr('err.' + code, code));

// A name given in several languages, {zh, en}, or as a plain string (taken as it is).
export function L(v) {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return v;
  return v[lang] ?? v.en ?? v.zh ?? Object.values(v)[0];
}

// An error of rpc(): its code translated, or the device's English message.
export function errText(e) {
  const k = 'err.' + (e && e.code || '');
  return dict[k] ?? en[k] ?? (e && e.message) ?? String(e);
}

export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => `&#${c.charCodeAt(0)};`);
}
