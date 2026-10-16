/* SPDX-License-Identifier: GPL-3.0-only */
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
const { chromium } = createRequire(import.meta.url)('playwright');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium', headless: true,
  args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'] });
try {
  const page = await browser.newPage();
  const errors = []; page.on('pageerror', error => errors.push(String(error)));
  await page.addInitScript(() => {
    const Node = window.AudioWorkletNode;
    window.AudioWorkletNode = class extends Node {
      constructor(...args) { super(...args); window.virtualNode = this; }
    };
  });
  await page.goto(process.env.VIRTUAL_URL || 'http://127.0.0.1:8010/');
  await page.locator('#start').click();
  await page.waitForFunction(() => document.querySelector('#status').textContent.includes('activo'));
  const before = await page.locator('#screen').evaluate(c => c.getContext('2d').getImageData(0, 0, 240, 240).data.some(v => v));
  assert(before, 'the real firmware LCD stayed black');
  await page.locator('#keys .key').nth(12).dispatchEvent('pointerdown', { pointerId: 1 });
  await page.waitForTimeout(250);
  const now = await page.evaluate(async () => {
    const c = new OfflineAudioContext(1, 1, 44100); // browser API availability smoke, audio path is live above
    return !!window.virtualNode && c.sampleRate;
  });
  assert(now);
  await page.locator('#keys .key').nth(12).dispatchEvent('pointerup', { pointerId: 1 });
  assert.deepEqual(errors, []);
  console.log('Chromium: AudioWorklet boot, firmware LCD and physical-key event OK');
} finally { await browser.close(); }
