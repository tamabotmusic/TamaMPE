/* SPDX-License-Identifier: GPL-3.0-only */
const status = document.querySelector('#status');
const start = document.querySelector('#start');
let context, synth;
const held = new Set();
const buttons = [];
function release(note, button) {
  if (held.delete(note)) synth.port.postMessage({ type: 'off', note });
  button.setAttribute('aria-pressed', 'false');
}
function releaseAll() { for (const button of buttons) release(Number(button.dataset.note), button); }
for (const [i, name] of ['Do', 'Re', 'Mi', 'Fa', 'Sol', 'La', 'Si', 'Do'].entries()) {
  const note = 60 + [0, 2, 4, 5, 7, 9, 11, 12][i];
  const button = document.createElement('button');
  button.textContent = name;
  button.dataset.note = note;
  button.disabled = true;
  button.setAttribute('aria-pressed', 'false');
  const on = () => {
    if (!synth || button.disabled || held.has(note)) return;
    held.add(note);
    button.setAttribute('aria-pressed', 'true');
    synth.port.postMessage({ type: 'on', note });
  };
  button.onpointerdown = event => { button.setPointerCapture(event.pointerId); on(); };
  button.onpointerup = button.onpointercancel = button.onlostpointercapture = () => release(note, button);
  button.onkeydown = event => { if (event.code === 'Space' || event.code === 'Enter') { event.preventDefault(); on(); } };
  button.onkeyup = event => { if (event.code === 'Space' || event.code === 'Enter') release(note, button); };
  button.onblur = () => release(note, button);
  buttons.push(button);
  document.querySelector('#keys').append(button);
}
window.addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
start.onclick = async () => {
  start.disabled = true;
  try {
    if (context) { await context.resume(); start.disabled = false; return; }
    context = new AudioContext({ latencyHint: 'interactive' });
    await context.resume();
    const response = await fetch('../../build/emulator/fm6.wasm');
    if (!response.ok) throw new Error('Compila primero el archivo fm6.wasm.');
    const bytes = await response.arrayBuffer();
    await context.audioWorklet.addModule('./processor.js');
    synth = new AudioWorkletNode(context, 'tamampe-fm6', { numberOfInputs: 0, outputChannelCount: [2] });
    synth.port.onmessage = ({ data }) => {
      if (data.type === 'ready') {
        status.textContent = `FM6 listo: ${data.rate} Hz / ${data.block} muestras → navegador ${context.sampleRate} Hz.`;
        buttons.forEach(button => { button.disabled = false; });
      } else if (data.type === 'error') status.textContent = data.message;
    };
    synth.connect(context.destination);
    // Raw bytes can cross the Worklet agent boundary without COOP/COEP.
    synth.port.postMessage({ type: 'load', bytes }, [bytes]);
    start.disabled = false;
    start.textContent = 'Reanudar audio';
  } catch (error) {
    status.textContent = String(error);
    if (context) await context.close();
    context = synth = null;
    start.disabled = false;
  }
};
