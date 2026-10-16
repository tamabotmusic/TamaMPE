/* SPDX-License-Identifier: GPL-3.0-only */
/* The AudioWorklet owns the real TamaMPE WebAssembly instance. */
class TamaMPEVirtualProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.lastDraw = -1;
    this.nextFrame = 0;
    this.port.onmessage = ({ data }) => this.message(data);
  }

  async message(data) {
    if (data.type === 'load') {
      try {
        const { instance } = await WebAssembly.instantiate(data.wasm, {});
        this.api = instance.exports;
        this.api._initialize?.();
        this.api.web_nor_erase();
        this.api.web_master(data.master ?? 700);
        this.api.web_boot();
        this.ready = true;
        this.port.postMessage({ type: 'ready', rate: this.api.web_sample_rate() });
      } catch (error) {
        this.port.postMessage({ type: 'error', message: String(error) });
      }
      return;
    }
    if (!this.ready) return;
    if (data.type === 'buttons') this.api.web_buttons(data.mask >>> 0);
    else if (data.type === 'keys') this.api.web_keys(data.mask >>> 0);
    else if (data.type === 'enc') this.api.web_enc(data.role >>> 0, data.steps | 0);
    else if (data.type === 'master') this.api.web_master(data.value >>> 0);
  }

  publish() {
    const draws = this.api.web_screen_draws();
    if (draws === this.lastDraw) return;
    this.lastDraw = draws;
    const fb = new Uint16Array(this.api.memory.buffer, this.api.web_screen(), 240 * 240).slice();
    this.port.postMessage({ type: 'frame', fb,
      litButtons: this.api.web_lit_buttons(), litKeys: this.api.web_lit_keys(),
      dimButtons: this.api.web_dim_buttons(), dimKeys: this.api.web_dim_keys() }, [fb.buffer]);
  }

  process(_inputs, outputs) {
    if (!this.ready) return true;
    const out = outputs[0], n = out[0].length;
    this.api.web_render(n);
    const mem = this.api.memory.buffer;
    const left = new Float32Array(mem, this.api.web_out_l(), n);
    const right = new Float32Array(mem, this.api.web_out_r(), n);
    out[0].set(left);
    if (out[1]) out[1].set(right);
    if (currentTime >= this.nextFrame) {
      this.nextFrame = currentTime + 1 / 30;
      this.publish();
    }
    return true;
  }
}
registerProcessor('tamampe-virtual', TamaMPEVirtualProcessor);
