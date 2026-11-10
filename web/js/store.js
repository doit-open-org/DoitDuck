// The latest status push and robot.info, for every page. Pages subscribe while mounted.
import {onStatus} from './rpc.js';

export const store = {status: null, info: null};
const fns = new Set();

onStatus(s => {
  store.status = s;
  fns.forEach(f => f(s));
});

// Calls f with every status push until the returned function is called.
export function subscribe(f) {
  fns.add(f);
  if (store.status) f(store.status);
  return () => fns.delete(f);
}

export const has = feature => !!store.info && store.info.features.includes(feature);
