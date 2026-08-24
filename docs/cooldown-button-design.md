# Cooldown Button Design

## Scope

The ddup repository currently contains a C RESP server and no frontend
runtime, component library, HTML entry point, or browser build. A button cannot
be safely added to the production tree until its host UI is identified. This
document defines the implementation contract so the eventual UI integration
does not invent incompatible state or security semantics.

## Performance-first model

- Store `readyAt` as a monotonic-clock deadline, never as a decrementing counter.
- Keep the button state in one small record: `readyAt`, `durationMs`, and a
  generation token. Checking readiness is O(1) and does not allocate.
- Use one shared animation frame scheduler for all cooldown buttons. The
  scheduler sleeps when no button is active and updates visible progress only
  once per frame; it must not create one timer per button.
- On completion, cancel the frame subscription and restore the enabled state.
- Persist only the action result or server-issued cooldown deadline when
  required; visual animation is derived state and is never authoritative.

## Safety and reliability

- The client disables the button optimistically, but the server remains the
  authority. Every action request carries an idempotency token.
- A rejected or rate-limited request must re-enable the button using the
  server response, not client wall-clock guesses.
- Cooldown checks use a monotonic deadline locally and a server timestamp or
  signed deadline remotely; wall-clock changes must not shorten a cooldown.
- Ignore stale responses using the generation token. A late success from a
  previous click must not re-enable a newer cooldown.
- Keyboard activation, assistive technology, and pointer activation must share
  the same guarded action path. The visual animation must expose an accessible
  progress label and not rely on color alone.
- The action endpoint must enforce authorization, replay protection, and rate
  limits independently of the button.

## TDD acceptance cases

1. Initial state is enabled and has no active animation.
2. A successful action sets `readyAt`, disables activation, and starts one
   shared animation subscription.
3. Repeated clicks, keyboard activation, and programmatic activation during
   cooldown issue no second action.
4. At the deadline the button becomes enabled and the animation subscription
   is removed.
5. A server rejection cancels the pending cooldown and exposes the reason.
6. A stale response cannot change a newer generation's state.
7. Clock rollback/forward does not make a locally active cooldown shorter than
   the server-provided deadline.
8. Destroying the component removes its scheduler subscription without leaks.

## Integration gate

When a frontend host is added, implement the state machine and tests in that
host's native framework, then add a browser-level test for animation and a
server integration test for idempotency/rate limiting. Until then this design
remains intentionally documentation-only.
