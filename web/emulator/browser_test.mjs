/* SPDX-License-Identifier: GPL-3.0-only */
// Optional integration check: npm-installed Playwright + Chromium, server running.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
const { chromium } = createRequire(import.meta.url)('playwright');
const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium',
  headless: true,
  args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'],
});
try {
  for (const rate of [44100, 48000]) {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.addInitScript(rate => {
      const Context = window.AudioContext;
      window.AudioContext = class extends Context {
        constructor(options) { super({ ...options, sampleRate: rate }); window.testContext = this; }
      };
      const Worklet = window.AudioWorkletNode;
      window.AudioWorkletNode = class extends Worklet {
        constructor(...args) {
          super(...args);
          window.testSynth = this;
          window.testAnalyser = window.testContext.createAnalyser();
          this.connect(window.testAnalyser);
        }
      };
    }, rate);
    await page.goto(process.env.EMULATOR_URL || 'http://localhost:8000/web/emulator/');
    await page.locator('#start').click();
    await page.waitForFunction(() => document.querySelector('#status').textContent.includes('FM6 listo'));
    const key = page.locator('#keys button').first();
    const bounds = await key.boundingBox();
    await page.mouse.move(bounds.x + 20, bounds.y + 20);
    await page.mouse.down();
    await page.waitForTimeout(300);
    const rms = await page.evaluate(() => {
      const values = new Float32Array(window.testAnalyser.fftSize);
      window.testAnalyser.getFloatTimeDomainData(values);
      return Math.sqrt(values.reduce((sum, x) => sum + x * x, 0) / values.length);
    });
    assert(rms > 0.0001, `Silent browser audio at ${rate} Hz`);
    await page.mouse.up();
    assert.equal(await key.getAttribute('aria-pressed'), 'false');
    // A lost window focus must release every held key.
    await page.mouse.down();
    await page.evaluate(() => window.dispatchEvent(new Event('blur')));
    assert.equal(await key.getAttribute('aria-pressed'), 'false');
    await page.mouse.up();
    assert.deepEqual(errors, []);
    console.log(`Chromium ${rate} Hz: actual AudioWorklet, Note On/Off, RMS=${rms}, focus cleanup OK`);
    await page.close();
  }
} finally { await browser.close(); }
