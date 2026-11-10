// A round stick: calls onMove(x, y) in -1..1 (right and up positive), and (0, 0) when let go.
export function joystick(el, onMove) {
  el.classList.add('stick');
  el.insertAdjacentHTML('afterbegin', '<i></i>');
  const knob = el.firstElementChild;
  let pointer = null;
  const reset = () => { pointer = null; knob.style.transform = ''; onMove(0, 0); };
  const move = e => {
    const b = el.getBoundingClientRect(), r = (b.width - knob.offsetWidth) / 2;
    let dx = e.clientX - b.left - b.width / 2, dy = e.clientY - b.top - b.height / 2;
    const d = Math.hypot(dx, dy);
    if (d > r) { dx *= r / d; dy *= r / d; }
    knob.style.transform = `translate(${dx}px,${dy}px)`;
    onMove(dx / r, -dy / r);
  };
  el.onpointerdown = e => { if (pointer !== null) return; pointer = e.pointerId; el.setPointerCapture(pointer); move(e); };
  el.onpointermove = e => { if (e.pointerId === pointer) move(e); };
  el.onpointerup = el.onpointercancel = e => { if (e.pointerId === pointer) reset(); };
  return reset;
}
