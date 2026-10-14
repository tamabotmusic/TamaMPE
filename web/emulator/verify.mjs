/* SPDX-License-Identifier: GPL-3.0-only */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const bytes = readFileSync('build/emulator/fm6.wasm');
const module = await WebAssembly.compile(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), [], 'Unexpected runtime imports');
const { exports: api } = await WebAssembly.instantiate(module, {});
if (api._initialize) api._initialize();
api.emulator_init();
assert.equal(api.emulator_rate(), 44100);
assert.equal(api.emulator_block(), 32);
const raw = readFileSync('build/emulator/reference.f32');
const reference = new Float32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4);
let difference = 0, energy = 0, peak = 0, maxError = 0, tailEnergy = 0;
for (let block = 0; block < 4096; block++) {
  if (block === 4) api.emulator_note_on(60);
  if (block === 512) api.emulator_note_on(64);
  if (block === 1024) api.emulator_note_off(60);
  if (block === 1536) api.emulator_note_off(64);
  const pcm = new Float32Array(api.memory.buffer, api.emulator_render(), 32);
  for (let i = 0; i < pcm.length; i++) {
    assert(Number.isFinite(pcm[i]) && Math.abs(pcm[i]) <= 1);
    const r = reference[block * 32 + i], delta = pcm[i] - r;
    difference += delta * delta;
    energy += r * r;
    peak = Math.max(peak, Math.abs(pcm[i]));
    maxError = Math.max(maxError, Math.abs(delta));
    if (block >= 3968) tailEnergy += pcm[i] ** 2;
  }
}
assert(peak > 0.001, 'Silent FM6');
assert.equal(tailEnergy, 0, 'Notes did not finish after Note Off');
const relativeError = Math.sqrt(difference / energy);
assert(relativeError < 0.001, `PCM regression: ${relativeError}`);
console.log(`FM6 WASM/native hostsim: 131072 samples, peak=${peak}, relative RMS error=${relativeError}, max error=${maxError}`);

// Exercise the actual Worklet adapter, including phase across render quanta.
globalThis.AudioWorkletProcessor = class {
  constructor() { this.port = { postMessage() {} }; }
};
globalThis.registerProcessor = () => {};
const { FM6Processor } = await import('./processor.js');
for (const rate of [22050, 44100, 48000, 96000]) {
  globalThis.sampleRate = rate;
  const processor = new FM6Processor();
  await processor.port.onmessage({ data: { type: 'load', bytes } });
  assert(processor.ready);
  await processor.port.onmessage({ data: { type: 'on', note: 60 } });
  const { exports: independent } = await WebAssembly.instantiate(module, {});
  if (independent._initialize) independent._initialize();
  independent.emulator_init();
  const source = new Float32Array(16384);
  source.set(new Float32Array(independent.memory.buffer, independent.emulator_render(), 32));
  independent.emulator_note_on(60);
  for (let at = 32; at < source.length; at += 32)
    source.set(new Float32Array(independent.memory.buffer, independent.emulator_render(), 32), at);
  let frame = 0, signalEnergy = 0;
  for (const quantum of [128, 64, 256, 128, 128, 256, 128, 128]) {
    const output = [new Float32Array(quantum), new Float32Array(quantum)];
    assert(processor.process([], [output]));
    for (let i = 0; i < quantum; i++, frame++) {
      const pos = frame * 44100 / rate, at = Math.floor(pos), f = pos - at;
      const expected = source[at] + (source[at + 1] - source[at]) * f;
      assert(Math.abs(output[0][i] - expected) < 1e-6, `Resampler ${rate}, frame ${frame}`);
      assert.equal(output[0][i], output[1][i]);
      signalEnergy += output[0][i] ** 2;
    }
  }
  assert(signalEnergy > 0);
  console.log(`AudioWorklet adapter: ${rate} Hz, variable quanta, stereo duplication OK`);
}
