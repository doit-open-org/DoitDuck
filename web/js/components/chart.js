// A few lines over the last seconds, e.g. joint targets while a new policy runs in preview.
export function chart(canvas, seconds = 6) {
  const lines = [];
  const colors = ['#f2b705', '#4dabf7', '#d6336c', '#2f9e44', '#ae3ec9', '#e67700'];
  return values => {
    const now = performance.now();
    lines.push([now, values]);
    while (lines.length && now - lines[0][0] > seconds * 1000) lines.shift();
    const w = canvas.width = canvas.clientWidth * devicePixelRatio, h = canvas.height = canvas.clientHeight * devicePixelRatio;
    const ctx = canvas.getContext('2d');
    const all = lines.flatMap(l => l[1]), lo = Math.min(...all, -1), hi = Math.max(...all, 1);
    ctx.lineWidth = devicePixelRatio * 1.5;
    values.forEach((_, k) => {
      ctx.strokeStyle = colors[k % colors.length];
      ctx.beginPath();
      lines.forEach(([t, v], i) => {
        const x = w - (now - t) / (seconds * 1000) * w, y = h - (v[k] - lo) / (hi - lo) * h;
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
      });
      ctx.stroke();
    });
  };
}
