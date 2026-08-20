# Phase 6 Problems and Marker Guidance

## Ownership

`MarkerService` is the process-local, HWND-free authority corresponding to the
stable part of VS Code's marker service. It owns accepted diagnostic
collections, resource identity, deterministic Problems snapshots, and change
notifications. Document navigation, editor decoration, problem matching, and
native Problems rendering are adapters outside this directory.

## State and Lifecycle Rules

- Identify every producer by stable owner ID and nonzero generation. Reloading
  an owner advances its generation; an older callback can never replace or
  delete newer markers.
- Replace is atomic for one collection/resource. Empty replacement is an
  explicit delete. Invalid, stale, oversized, or revision-conflicting requests
  preserve the last accepted snapshot.
- Ranges are zero-based and half-open. Preserve URI identity and the complete
  start/end range through navigation; opening only the resource is not a
  compatible Problems action.
- Keep collection, resource, marker, payload, subscription, and queued
  notification work bounded. Snapshot ordering must not depend on insertion or
  callback order.
- Commit under the model lock and deliver queued notifications after unlocking.
  Listener reentrancy and exceptions must not strand delivery or mutate the
  already committed result.
- Bound retained owner identities as well as marker collections. A tombstoned
  owner generation is a lifetime fence and must not be evicted in a way that
  lets stale callbacks become current again.
- `Stop` is terminal. An external caller waits for active callbacks to drain;
  a callback-originated Stop returns the typed deferred result and transfers
  finalization to the safe outer delivery boundary. Service destruction from
  inside its own callback is unsupported because the callback still borrows the
  service. Stop clears retained data/subscribers and rejects every later
  mutation. Adapters must dispose only the exact owner generation they created.

## Verified Checkpoint

`CWorkbenchRuntime` owns `MarkerService`; `CEditWnd` projects authoritative
snapshots through a coalesced UI-thread gate; and extension diagnostic RPC
updates the service before best-effort legacy decoration projection. Atomic
collection clear removes only the exact owner generation and collection with
one committed revision. Missing/repeated delete, clear, and empty-set requests
are accepted desired-absence operations so legacy decorations cannot remain
after the authoritative marker is gone.

The integrated Phase 6 cohort passes 210/210. `workbench::tasks::ProblemMatcherEngine`
(`sakura_core/workbench/tasks/ProblemMatcherEngine.h`/`.cpp`) can now translate
captured task output lines into `ReplaceMarkersRequest` values a caller hands to
this service, but Task execution does not yet call it: no production adapter
resolves a task's `problemMatchers` names, feeds live output through the engine,
and calls `Replace` here, so Task problem matchers remain functionally unwired
end to end. Native activation deliberately opens only the resource for now:
full-range navigation and decorations must first convert VS Code's zero-based,
half-open UTF-16 code-unit positions to Sakura document coordinates without
truncating the stored URI/range.

## Position adapter (`MarkerPositionAdapter.h`/`.cpp`, 2026-08-20, #221)

- `ConvertMarkerPositionToLogicPoint` / `ConvertMarkerRangeStartToLogicPoint` /
  `ConvertMarkerRangeToLogicRange` are the seam the paragraph above asks for.
  They are pure: no HWND, no `CEditDoc`, no I/O. The only document-dependent
  input is the injected `LogicLineContentLookup` callback, which the caller
  backs with the real document; the conversion math itself is a function of
  its arguments and is unit tested without constructing any document object.
- **No unit rescale is needed.** `CDocLine` already stores a line as a
  `wchar_t` array (one element per UTF-16 code unit), and `CLogicInt`/
  `CLogicPoint` already count logic positions in that same array — the
  existing `CViewCommander::Command_TagJumpNoMessage` tag-jump path and
  `CEditWnd`'s `MYWM_SETCARETPOS` handler both already set a `CLogicPoint`
  straight from an externally parsed line/column pair with only a 1-based to
  0-based `-1` offset, never a width rescale. A surrogate pair is therefore
  two logic units, matching both VS Code's own `Position.character` unit and
  Sakura's own storage; the adapter must not "fix" that into one code point.
- **Tab expansion is out of scope by construction.** Logic coordinates are
  pre-tab-expansion; that is the entire Logic/Layout distinction
  (`sakura_core/doc/CLAUDE.md`, `CLayoutMgr::LogicToLayout`). A converted
  `CLogicPoint` needs no tab-width adjustment of its own — the caller runs it
  through the same `LogicToLayout` conversion a tag-jump landing position
  already goes through before `MYWM_SETCARETPOS` applies it.
- **Chosen out-of-range behavior: clamp, not a typed failure.** VS Code's own
  `TextModel.validatePosition` (`PieceTreeTextBuffer.validatePosition`) clamps
  an out-of-bounds line to the last line and an out-of-bounds column to that
  line's length; it does not throw or drop the request. A Problems entry can
  easily outlive the exact document snapshot the diagnostic was computed
  against (edited or reloaded between diagnostic and double-click), so
  matching VS Code's clamp is the compatible behavior — rejecting the
  activation outright would make a marker un-activatable the moment the
  document drifted by one line, which real VS Code does not do.
  `MarkerPositionClamp` reports which axis moved, for a caller that wants to
  surface a "position no longer exists exactly" hint; it is not itself an
  error result.
- **Chosen surrogate-safety behavior, also matching `validatePosition`:** a
  clamped column is additionally never left strictly inside a UTF-16
  surrogate pair. If it would land between a high surrogate and its low
  surrogate, it moves back one unit, in front of the pair, and is reported as
  clamped.
- Native wiring (reading `problem.range`, calling this adapter with the real
  document's line content, and applying the resulting `CLogicPoint` through
  `CEditWnd`) is intentionally not part of this file's ownership; see
  `window/CLAUDE.md` for where that native seam lives.
