// An ONNX policy to the robot's weight file, in the browser, as tools/import_policy.py does it:
// the graph must be (obs - mean) / std -> Gemm -> Elu -> Gemm -> Elu -> Gemm -> Elu -> Gemm,
// 61 -> 512 -> 256 -> 128 -> 14; the file is float32 little endian: mean, std, then weight and
// bias of each layer. Only the protobuf this needs is read.

const SHAPES = [[61], [61], [512, 61], [512], [256, 512], [256], [128, 256], [128], [14, 128], [14]];
const OPS = ['Sub', 'Div', 'Gemm', 'Elu', 'Gemm', 'Elu', 'Gemm', 'Elu', 'Gemm'];

function varint(b, i) {
  let r = 0, s = 1, c;
  do { c = b[i++]; r += (c & 0x7f) * s; s *= 128; } while (c >= 0x80);
  return [r, i];
}

// Fields of a protobuf message: [field number, wire type, value (a number or a byte view)].
function* fields(b) {
  let i = 0;
  while (i < b.length) {
    let k, v, n;
    [k, i] = varint(b, i);
    const f = Math.floor(k / 8), w = k & 7;
    if (w === 0) [v, i] = varint(b, i);
    else if (w === 1) { v = b.subarray(i, i + 8); i += 8; }
    else if (w === 2) { [n, i] = varint(b, i); v = b.subarray(i, i + n); i += n; }
    else if (w === 5) { v = b.subarray(i, i + 4); i += 4; }
    else throw new Error('protobuf wire type ' + w);
    yield [f, w, v];
  }
}

const text = b => new TextDecoder().decode(b);
const f32 = b => new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true);

function tensor(b) {
  let name = '', dims = [], type = 1, raw = null;
  const floats = [];
  for (const [f, w, v] of fields(b)) {
    if (f === 1 && w === 0) dims.push(v);
    else if (f === 1) for (let i = 0; i < v.length;) { let x; [x, i] = varint(v, i); dims.push(x); }
    else if (f === 2) type = v;
    else if (f === 4 && w === 5) floats.push(v);
    else if (f === 4) for (let i = 0; i < v.length; i += 4) floats.push(v.subarray(i, i + 4));
    else if (f === 8) name = text(v);
    else if (f === 9) raw = v;
    else if (f === 14) throw new Error(`${name}: external data is not supported`);
  }
  if (type !== 1) throw new Error(`${name}: not float32`);
  const data = raw || new Uint8Array(floats.flatMap(x => [...x]));
  return {name, dims, data};
}

// Returns {weights: Uint8Array, checks: [[key, ok, detail]]}; weights is null when a check failed.
export function convert(buffer) {
  const checks = [], b = new Uint8Array(buffer);
  let graph = null;
  for (const [f, , v] of fields(b)) if (f === 7) graph = v;
  if (!graph) return {weights: null, checks: [['onnx.graph', false, 'no graph']]};
  const nodes = [], init = {};
  for (const [f, , v] of fields(graph)) {
    if (f === 1) {
      const node = {inputs: [], attrs: {}};
      for (const [nf, nw, nv] of fields(v)) {
        if (nf === 1) node.inputs.push(text(nv));
        else if (nf === 4) node.op = text(nv);
        else if (nf === 5) {
          let name = '', val;
          for (const [af, aw, av] of fields(nv)) {
            if (af === 1) name = text(av);
            else if (af === 2 && aw === 5) val = f32(av);
            else if (af === 3 && aw === 0) val = av;
          }
          node.attrs[name] = val;
        }
      }
      nodes.push(node);
    } else if (f === 5) {
      const t = tensor(v);
      init[t.name] = t;
    }
  }
  const ops = nodes.map(n => n.op);
  const graphOk = ops.join() === OPS.join() && nodes.every(n => n.op !== 'Gemm' ||
    (n.attrs.transB === 1 && (n.attrs.alpha ?? 1) === 1 && (n.attrs.beta ?? 1) === 1 && !n.attrs.transA)) &&
    nodes.every(n => n.op !== 'Elu' || (n.attrs.alpha ?? 1) === 1);
  checks.push(['onnx.graph', graphOk, ops.join(' → ')]);
  if (!graphOk) return {weights: null, checks};
  const names = [nodes[0].inputs[1], nodes[1].inputs[1], ...nodes.filter(n => n.op === 'Gemm').flatMap(n => n.inputs.slice(1, 3))];
  const parts = [];
  let shapesOk = true;
  names.forEach((name, k) => {
    const t = init[name], want = SHAPES[k];
    let dims = t ? t.dims : [];
    if (dims.length === want.length + 1 && dims[0] === 1) dims = dims.slice(1); // mean and std may be [1, 61]
    const ok = !!t && dims.join() === want.join() && t.data.length === 4 * want.reduce((a, c) => a * c, 1);
    shapesOk = shapesOk && ok;
    if (ok) parts.push(t.data);
  });
  checks.push(['onnx.shapes', shapesOk, '61 → 512 → 256 → 128 → 14']);
  if (!shapesOk) return {weights: null, checks};
  const weights = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { weights.set(p, at); at += p.length; }
  return {weights, checks};
}

// The manifest checks the robot makes too (docs/doitduck-design.md §6.2): [[key, ok, detail]].
export function checkManifest(m, compatible) {
  const differs = (k, want) => m[k] !== undefined && m[k] !== want;
  const cmd = m.command || {}, enc = cmd.encoding || 'constant', kind = m.kind || '';
  const model = m.robot && m.robot.model;
  const supported = (kind === 'perpetual' && enc === 'constant' && ['walk', 'stand'].includes(m.slot)) ||
    (kind === 'episodic' && ['constant', 'phase'].includes(enc)) || (kind === 'scripted' && enc === 'posture_flag');
  return [
    ['manifest.api', !differs('model_api', 1) && !differs('obs_len', 61) && !differs('action_len', 14), `model_api ${m.model_api ?? '-'}, ${m.obs_len ?? '-'} → ${m.action_len ?? '-'}`],
    ['manifest.model', compatible.includes(model), `${model || '-'} / ${compatible.join(', ')}`],
    ['manifest.kind', supported, `${kind || '-'} · ${enc}${m.slot ? ' · ' + m.slot : ''}`],
    ['manifest.scale', !differs('action_scale', 1), String(m.action_scale ?? 1)],
  ];
}

export function base64(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}
