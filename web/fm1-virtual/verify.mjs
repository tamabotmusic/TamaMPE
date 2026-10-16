/* SPDX-License-Identifier: GPL-3.0-only */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const { instance } = await WebAssembly.instantiate(readFileSync('build/fm1-virtual/tamampe-virtual.wasm'), {});
const api = instance.exports;
api._initialize?.(); api.web_nor_erase(); api.web_boot();
assert.equal(api.web_sample_rate(), 44100);
for (let i = 0; i < 32; i++) api.web_render(128);
assert(api.web_screen_draws() > 0, 'firmware UI did not draw');
const { instance: referenceInstance } = await WebAssembly.instantiate(readFileSync('build/fm1-virtual/tamampe-virtual.wasm'), {});
const reference = referenceInstance.exports; reference._initialize?.(); reference.web_nor_erase(); reference.web_boot();
const raw = readFileSync('build/fm1-virtual/reference.f32');
const native = new Float32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4);
let energy = 0, at = 0, error = 0;
for (let block = 0; block < 512; block++) {
  if (block === 16) { api.web_keys(1 << 12); reference.web_keys(1 << 12); }
  if (block === 256) { api.web_keys(0); reference.web_keys(0); }
  api.web_render(128); reference.web_render(128);
  const pcm = new Float32Array(api.memory.buffer, api.web_out_l(), 128);
  const ref = new Float32Array(reference.memory.buffer, reference.web_out_l(), 128);
  for (let i = 0; i < 128; i++, at++) { energy += pcm[i] * pcm[i]; error = Math.max(error, Math.abs(pcm[i] - native[at]), Math.abs(pcm[i] - ref[i])); }
}
api.web_buttons(1 << 10); api.web_render(128); api.web_buttons(0);
assert(energy > 1e-6, 'physical-key input did not reach the firmware audio path');
assert(api.web_lit_keys() !== undefined && api.web_lit_buttons() !== undefined);
assert(error < 1e-6, `PCM differs from native/reference: ${error}`);
console.log(`TamaMPE virtual: real UI draws=${api.web_screen_draws()}, key audio energy=${energy}, PCM max error=${error}`);
