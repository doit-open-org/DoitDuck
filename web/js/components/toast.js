// A short message at the bottom; an rpc() error shows translated.
import {errText} from '../i18n.js';

let timer = 0;

export function toast(msg) {
  const el = document.getElementById('toast');
  el.textContent = msg instanceof Error ? errText(msg) : msg;
  el.classList.add('show');
  clearTimeout(timer);
  timer = setTimeout(() => el.classList.remove('show'), 3000);
}

// Runs an rpc() promise, showing why it failed; resolves to undefined then.
export const act = p => p.catch(toast);
