const assert = require('assert');
const { CooldownButtonController } = require('./cooldown_button');

class FakeScheduler {
  constructor() { this.cb = null; this.starts = 0; this.stops = 0; }
  subscribe(cb) { this.cb = cb; this.starts++; return () => { this.cb = null; this.stops++; }; }
  frame(now) { if (this.cb) this.cb(now); }
}

async function test_cooldown_and_animation() {
  const scheduler = new FakeScheduler();
  const frames = [];
  const button = new CooldownButtonController({ scheduler, onRender: state => frames.push(state) });
  assert.strictEqual(button.canActivate(100), true);
  const result = await button.activate(100, async () => ({ cooldownMs: 1000 }));
  assert.strictEqual(result.accepted, true);
  assert.strictEqual(button.canActivate(500), false);
  assert.strictEqual(scheduler.starts, 1);
  scheduler.frame(500);
  assert.strictEqual(frames.at(-1).remainingMs, 600);
  scheduler.frame(1100);
  assert.strictEqual(button.canActivate(1100), true);
  assert.strictEqual(scheduler.stops, 1);
}

async function test_duplicate_and_stale_activation() {
  const scheduler = new FakeScheduler();
  const button = new CooldownButtonController({ scheduler });
  let resolve;
  const pending = new Promise(r => { resolve = r; });
  const first = button.activate(0, () => pending);
  const second = await button.activate(1, async () => ({ cooldownMs: 1 }));
  assert.strictEqual(second.accepted, false);
  resolve({ cooldownMs: 10 });
  assert.strictEqual((await first).accepted, true);
  assert.strictEqual(button.canActivate(5), false);
}

async function test_rejection_does_not_start_cooldown() {
  const scheduler = new FakeScheduler();
  const button = new CooldownButtonController({ scheduler });
  const result = await button.activate(50, async () => { throw new Error('denied'); });
  assert.strictEqual(result.accepted, false);
  assert.strictEqual(button.canActivate(50), true);
  assert.strictEqual(scheduler.starts, 0);
}

(async () => {
  await test_cooldown_and_animation();
  await test_duplicate_and_stale_activation();
  await test_rejection_does_not_start_cooldown();
  console.log('cooldown button tests: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
