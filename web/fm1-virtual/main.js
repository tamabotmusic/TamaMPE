/* SPDX-License-Identifier: GPL-3.0-only */
const B = ['FX', 'SCL', 'ENV', 'LFO', 'EDIT', 'GLO', 'HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC', 'OCT-', 'OCT+'];
const E = ['SELECT', 'ALGORITHM', 'PRESETS', 'KNOB 1', 'KNOB 2', 'KNOB 3', 'KNOB 4'];
const status = document.querySelector('#status');
const canvas = document.querySelector('#screen');
const ctx = canvas.getContext('2d');
const image = ctx.createImageData(240, 240);
let node, buttons = 0, keys = 0;
const buttonEls = [], keyEls = [];

function send(message) { node?.port.postMessage(message); }
function draw(fb) {
  const p = image.data;
  for (let i = 0; i < fb.length; i++) {
    const rgb = ((fb[i] >> 8) | (fb[i] << 8)) & 0xffff;
    p[4 * i] = ((rgb >> 11) & 31) * 255 / 31;
    p[4 * i + 1] = ((rgb >> 5) & 63) * 255 / 63;
    p[4 * i + 2] = (rgb & 31) * 255 / 31;
    p[4 * i + 3] = 255;
  }
  ctx.putImageData(image, 0, 0);
}
function setBit(kind, bit, on) {
  if (kind === 'button') { buttons = on ? buttons | (1 << bit) : buttons & ~(1 << bit); send({ type: 'buttons', mask: buttons }); }
  else { keys = on ? keys | (1 << bit) : keys & ~(1 << bit); send({ type: 'keys', mask: keys }); }
}
function bindPress(element, kind, bit) {
  element.addEventListener('pointerdown', event => { element.setPointerCapture(event.pointerId); setBit(kind, bit, true); });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) element.addEventListener(type, () => setBit(kind, bit, false));
}
function renderLeds(elements, lit, dim) {
  for (const [bit, el] of elements.entries()) {
    el.classList.toggle('lit', !!(lit & (1 << bit)));
    el.classList.toggle('dim', !(lit & (1 << bit)) && !!(dim & (1 << bit)));
  }
}
for (const [i, label] of B.entries()) {
  const el = document.createElement('button'); el.textContent = label; el.className = 'button';
  bindPress(el, 'button', i); buttonEls.push(el); document.querySelector('#buttons').append(el);
}
for (let i = 0; i < 27; i++) {
  const el = document.createElement('button'); el.textContent = ['F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B', 'C', 'C♯', 'D', 'D♯', 'E'][i % 12];
  el.className = i % 12 === 1 || i % 12 === 3 || i % 12 === 6 || i % 12 === 8 || i % 12 === 10 ? 'key black' : 'key';
  bindPress(el, 'key', i); keyEls.push(el); document.querySelector('#keys').append(el);
}
for (const [i, label] of E.entries()) {
  const el = document.createElement('button'); el.textContent = label; el.className = 'encoder';
  let y;
  el.addEventListener('pointerdown', event => { y = event.clientY; el.setPointerCapture(event.pointerId); });
  el.addEventListener('pointermove', event => { if (y === undefined) return; const steps = Math.trunc((y - event.clientY) / 8); if (steps) { send({ type: 'enc', role: i, steps }); y -= steps * 8; } });
  el.addEventListener('pointerup', () => { y = undefined; });
  el.addEventListener('wheel', event => { event.preventDefault(); send({ type: 'enc', role: i, steps: event.deltaY < 0 ? 1 : -1 }); }, { passive: false });
  document.querySelector('#encoders').append(el);
}
document.querySelector('#master').addEventListener('input', event => send({ type: 'master', value: +event.target.value }));
window.addEventListener('blur', () => { buttons = keys = 0; send({ type: 'buttons', mask: 0 }); send({ type: 'keys', mask: 0 }); });
document.querySelector('#start').addEventListener('click', async () => {
  const start = document.querySelector('#start'); start.disabled = true;
  try {
    const ac = new AudioContext({ latencyHint: 'interactive' });
    const wasm = await fetch('./tamampe-virtual.wasm').then(r => { if (!r.ok) throw Error(`WASM ${r.status}`); return r.arrayBuffer(); });
    await ac.audioWorklet.addModule('./worklet.js');
    node = new AudioWorkletNode(ac, 'tamampe-virtual', { numberOfInputs: 0, outputChannelCount: [2] });
    node.port.onmessage = ({ data }) => {
      if (data.type === 'ready') status.textContent = `Firmware TamaMPE activo · ${data.rate} Hz`;
      if (data.type === 'frame') { draw(data.fb); renderLeds(buttonEls, data.litButtons, data.dimButtons); renderLeds(keyEls, data.litKeys, data.dimKeys); }
      if (data.type === 'error') status.textContent = data.message;
    };
    node.connect(ac.destination); node.port.postMessage({ type: 'load', wasm, master: 700 }, [wasm]);
    await ac.resume(); start.hidden = true;
  } catch (error) { status.textContent = String(error); start.disabled = false; }
});
