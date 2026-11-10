// The torso as an attitude indicator: the horizon turns against the roll and rises as the nose goes
// down; the tilt from upright is written under the reticle.
export const attitudeHtml = () => `<div class="attitude"><div class="sky"></div>
  <svg class="reticle" viewBox="0 0 100 100"><path d="M22 50h18l6 6 4-6 4 6 6-6h18"/><circle cx="50" cy="50" r="2"/></svg><span class="tilt num"></span></div>`;

// Roll and pitch in degrees from the gravity vector of robot.status.
export function rollPitch(g) {
  const [gx, gy, gz] = g;
  return [Math.atan2(-gy, -gz) * 180 / Math.PI, Math.atan2(gx, -gz) * 180 / Math.PI];
}

export function showAttitude(el, s) {
  const [roll, pitch] = s.imu_ok ? rollPitch(s.gravity) : [0, 0];
  el.querySelector('.attitude .sky').style.transform = `rotate(${-roll}deg) translateY(${-pitch * 1.2}px)`;
  el.querySelector('.attitude .tilt').textContent = s.imu_ok && s.tilt_deg != null ? s.tilt_deg.toFixed(1) + '°' : '—';
}
