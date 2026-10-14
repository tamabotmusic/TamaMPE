/* SPDX-License-Identifier: GPL-3.0-only */
export class FM6Processor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.fraction = 0;
    this.index = 0;
    this.port.onmessage = async ({ data }) => {
      if (data.type === 'load') {
        try {
          const result = await WebAssembly.instantiate(data.bytes || data.module, {});
          this.api = (result.instance || result).exports;
          if (this.api._initialize) this.api._initialize();
          this.api.emulator_init();
          this.size = this.api.emulator_block();
          this.ratio = this.api.emulator_rate() / sampleRate;
          this.pcm = new Float32Array(this.api.memory.buffer,
            this.api.emulator_render(), this.size);
          this.left = this.nextSample();
          this.right = this.nextSample();
          this.ready = true;
          this.port.postMessage({ type: 'ready', rate: this.api.emulator_rate(), block: this.size });
        } catch (error) {
          this.port.postMessage({ type: 'error', message: String(error) });
        }
      } else if (this.ready && data.type === 'on') {
        this.api.emulator_note_on(data.note);
      } else if (this.ready && data.type === 'off') {
        this.api.emulator_note_off(data.note);
      }
    };
  }
  nextSample() {
    if (this.index === this.size) {
      this.api.emulator_render();
      this.index = 0;
    }
    return this.pcm[this.index++];
  }
  process(_inputs, outputs) {
    const channels = outputs[0];
    if (!this.ready) return true; // Web Audio provides zeroed output buffers.
    for (let i = 0; i < channels[0].length; i++) {
      const value = this.left + (this.right - this.left) * this.fraction;
      for (let ch = 0; ch < channels.length; ch++) channels[ch][i] = value;
      this.fraction += this.ratio;
      while (this.fraction >= 1) {
        this.fraction -= 1;
        this.left = this.right;
        this.right = this.nextSample();
      }
    }
    return true;
  }
}
registerProcessor('tamampe-fm6', FM6Processor);
