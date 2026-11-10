// One WebSocket to /ws: JSON-RPC requests out, replies and a status push every 100 ms in.
// Big requests (an imported policy, a recording) go as POST /api/<method>.
// A refused request rejects with an Error whose .code is the device's error code.

let ws = null, seq = 0;
const pending = new Map(), statusFns = new Set(), connFns = new Set();

function fail(e) {
  const err = new Error(e && e.message || String(e));
  err.code = e && e.code || '';
  return err;
}

export function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => connFns.forEach(f => f(true));
  ws.onclose = () => {
    connFns.forEach(f => f(false));
    pending.forEach(p => p.reject(fail({code: 'disconnected', message: 'disconnected'})));
    pending.clear();
    setTimeout(connect, 1000);
  };
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.type === 'status') return statusFns.forEach(f => f(m.data));
    const p = pending.get(m.id);
    if (!p) return;
    pending.delete(m.id);
    m.error !== undefined ? p.reject(fail(m.error)) : p.resolve(m.result);
  };
}

export const connected = () => ws && ws.readyState === 1;

export function rpc(method, params = {}) {
  return new Promise((resolve, reject) => {
    if (!connected()) return reject(fail({code: 'disconnected', message: 'not connected'}));
    const id = ++seq;
    pending.set(id, {resolve, reject});
    ws.send(JSON.stringify({id, method, params}));
    setTimeout(() => { if (pending.delete(id)) reject(fail({code: 'timeout', message: 'timeout'})); }, 8000);
  });
}

export async function post(method, params = {}) {
  const r = await fetch(`/api/${method}`, {method: 'POST', body: JSON.stringify(params)});
  const m = await r.json();
  if (m.error) throw fail(m.error);
  return m.result;
}

export const onStatus = f => (statusFns.add(f), () => statusFns.delete(f));
export const onConn = f => (connFns.add(f), () => connFns.delete(f));
