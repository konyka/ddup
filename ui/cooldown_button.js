'use strict';

class SharedFrameScheduler {
  constructor(frame = cb => setTimeout(() => cb(
    typeof performance !== 'undefined' ? performance.now() : Date.now()), 16)) {
    this.frame = frame;
    this.listeners = new Set();
    this.running = false;
    this.tick = now => {
      if (!this.running) return;
      for (const listener of this.listeners) listener(now);
      if (this.listeners.size === 0) {
        this.running = false;
        return;
      }
      this.frame(this.tick);
    };
  }

  subscribe(listener) {
    this.listeners.add(listener);
    if (!this.running) {
      this.running = true;
      this.frame(this.tick);
    }
    return () => {
      this.listeners.delete(listener);
      if (this.listeners.size === 0) this.running = false;
    };
  }
}

class CooldownButtonController {
  constructor({ scheduler, onRender = () => {} } = {}) {
    this.scheduler = scheduler || new SharedFrameScheduler();
    this.onRender = onRender;
    this.readyAt = 0;
    this.durationMs = 0;
    this.generation = 0;
    this.inFlight = false;
    this.unsubscribe = null;
  }

  canActivate(now) {
    return !this.inFlight && now >= this.readyAt;
  }

  state(now) {
    const remainingMs = Math.max(0, this.readyAt - now);
    return {
      disabled: this.inFlight || remainingMs > 0,
      remainingMs,
      durationMs: this.durationMs,
      progress: this.durationMs > 0 ? Math.min(1, remainingMs / this.durationMs) : 0,
    };
  }

  _render(now) {
    const state = this.state(now);
    this.onRender(state);
    if (!state.disabled && this.unsubscribe) {
      this.unsubscribe();
      this.unsubscribe = null;
    }
  }

  _startAnimation(generation) {
    if (this.unsubscribe) this.unsubscribe();
    this.unsubscribe = this.scheduler.subscribe(now => {
      if (generation !== this.generation) return;
      this._render(now);
    });
  }

  async activate(now, action) {
    if (!this.canActivate(now)) return { accepted: false, reason: 'cooldown' };
    const generation = ++this.generation;
    this.inFlight = true;
    this._render(now);
    try {
      const response = await action();
      if (generation !== this.generation) return { accepted: false, reason: 'stale' };
      const cooldownMs = Number(response && response.cooldownMs);
      if (!Number.isFinite(cooldownMs) || cooldownMs < 0) {
        this.inFlight = false;
        this._render(now);
        return { accepted: false, reason: 'invalid-cooldown' };
      }
      this.inFlight = false;
      this.durationMs = cooldownMs;
      this.readyAt = now + cooldownMs;
      if (cooldownMs > 0) this._startAnimation(generation);
      this._render(now);
      return { accepted: true, cooldownMs };
    } catch (error) {
      if (generation === this.generation) {
        this.inFlight = false;
        this._render(now);
      }
      return { accepted: false, reason: 'rejected', error };
    }
  }

  destroy() {
    this.generation++;
    this.inFlight = false;
    if (this.unsubscribe) this.unsubscribe();
    this.unsubscribe = null;
  }
}

module.exports = { SharedFrameScheduler, CooldownButtonController };
