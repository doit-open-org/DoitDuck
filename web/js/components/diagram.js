// The robot as robots/<name>.json "diagram" draws it, front view: its parts ("shapes": rect [x, y, w, h, rx],
// circle [cx, cy, r] or ellipse [cx, cy, rx, ry], each with a class), brackets between servos ("bones"), side
// tags, and each servo with its ID on it: a block [x, y, w, h] around its center, or a horn [x, y, r].
// classes(i) gives a servo its class: hi (the joint in question), ok, bad, off.
import {store} from '../store.js';
import {L} from '../i18n.js';

export function diagramHtml(classes = () => '') {
  const g = store.info && store.info.diagram;
  if (!g) return '';
  const names = L(store.info.motor_names) || [], ids = store.info.motors, p = g.points;
  const shapes = (g.shapes || []).map(s => s.rect ? `<rect x="${s.rect[0]}" y="${s.rect[1]}" width="${s.rect[2]}" height="${s.rect[3]}" rx="${s.rect[4] || 0}" class="${s.class}"/>`
    : s.circle ? `<circle cx="${s.circle[0]}" cy="${s.circle[1]}" r="${s.circle[2]}" class="${s.class}"/>`
    : `<ellipse cx="${s.ellipse[0]}" cy="${s.ellipse[1]}" rx="${s.ellipse[2]}" ry="${s.ellipse[3]}" class="${s.class}"/>`).join('');
  const bones = g.bones.map(([a, b]) => `<line x1="${p[a][0]}" y1="${p[a][1]}" x2="${p[b][0]}" y2="${p[b][1]}"/>`).join('');
  const tags = (g.tags || []).map(t => {
    const text = L(t.text), w = [...text].reduce((n, c) => n + (c.charCodeAt(0) > 255 ? 7 : 4.2), 10);
    return `<g class="tag"><rect x="${t.end ? t.x - w : t.x}" y="${t.y - 7}" width="${w}" height="14" rx="7"/>
      <text x="${t.end ? t.x - w / 2 : t.x + w / 2}" y="${t.y}">${text}</text></g>`;
  }).join('');
  const servos = p.map(([x, y, w, h], i) => {
    const part = h ? (pad => `<rect x="${x - w / 2 - pad}" y="${y - h / 2 - pad}" width="${w + 2 * pad}" height="${h + 2 * pad}" rx="${3 + pad}"`)
      : (pad => `<circle cx="${x}" cy="${y}" r="${w + pad}"`);
    return `<g class="j ${h ? 'block' : 'horn'} ${classes(i)}"><title>${names[i] || i} (ID ${ids[i]})</title>
      ${part(4)} class="halo"/>${part(0)}/><text x="${x}" y="${y}">${ids[i]}</text></g>`;
  }).join('');
  return `<svg class="duck" viewBox="0 0 ${g.size[0]} ${g.size[1]}">${shapes}${bones}${tags}${servos}</svg>`;
}
