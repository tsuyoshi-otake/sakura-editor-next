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

`CWorkbenchRuntime` owns `MarkerService`; `CEditWnd` borrows it only to
subscribe through a coalesced UI-thread gate and to project authoritative
snapshots. The first production producer is wired in #380:
`workbench::tasks::TaskProblemCollector` resolves a task's configured
`problemMatchers` names through `BuiltinProblemMatchers`, feeds the run's
drained terminal output through `ProblemMatcherEngine`, and applies the
resulting `ReplaceMarkersRequest` values here under the matcher owner and a
per-run generation from `CTaskMarkerGenerationBook`. The book also tracks the
last-delivered resource set per owner so a rerun replaces prior markers and
clears resources the new run no longer reports, matching upstream
`problemCollectors.ts` `changeOne`/`remove` semantics. The extension
diagnostic RPC removed in #169 remains absent. Atomic collection clear
removes only the exact owner generation and collection with one committed
revision. Missing/repeated delete, clear, and empty-set requests are accepted
desired-absence operations so legacy decorations cannot remain after the
authoritative marker is gone.

The integrated Phase 6 cohort passes 210/210. Native activation now applies
the marker's range start:
`CEditWnd::OpenDocumentAtMarkerPosition` converts the stored zero-based UTF-16
line/column through the `MarkerPositionAdapter` below against the real
document's line contents before posting `MYWM_SETCARETPOS`. Editor decorations
from markers remain unimplemented.

## Position adapter (`MarkerPositionAdapter.h`/`.cpp`, 2026-08-20, #221)

- `ConvertMarkerPositionToLogicPoint` / `ConvertMarkerRangeStartToLogicPoint` /
  `ConvertMarkerRangeToLogicRange` are the seam the paragraph above names.
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

## Marker navigation model (`MarkerNavigationModel.h`/`.cpp`, #382)

`CMarkerNavigationModel` is the pure port of upstream VS Code
`markerNavigationService.ts` `MarkerList`: it owns a sorted marker run plus the
`_nextIdx` move/init/reset semantics behind `editor.action.marker.nextInFiles`,
`prevInFiles`, `next`, and `prev`. It has no HWND, no document, and no
subscription; callers feed it `MarkerService::Snapshot()` output.

- `BuildMarkerNavigationMarkers` flattens a snapshot with the upstream
  `severities: Error|Warning|Info` filter (Hint markers are out) and the
  upstream `compareMarker` order: resource URI ordinal first, then
  severity→start (`problems.sortOrder = "severity"`, the default) or
  start→severity (`"position"`). The optional resource filter is
  `extUri.isEqual`-equivalent, not a string prefix.
- `Matches` mirrors upstream `matches(uri)` exactly: a filterless model (the
  `*InFiles` session) matches only a filterless (`nullopt`) query — a following
  same-file command builds a fresh URI-filtered model with the `reusePosition`
  step instead; a URI-filtered model matches only `toString() ===` string-equal
  URIs (not `isEqual`; both sides are canonical `Uri::ToString()` strings).
- `Move` reproduces upstream `_initIdx`/`move`: first move finds the first
  `isEqual` resource run, resolves the marker containing the caret (empty
  ranges are expanded through the injected `WordAtPosition`, the
  `model.getWordAtPosition` analog) or the next marker start it went past —
  with the one-step-back correction on backward — and wraps to 0 / last when
  the resource's markers are all behind. A resource with no markers resolves
  the string-ordinal insertion index. Later moves only advance/wrap `_nextIdx`.
  The query URI is canonicalized to `Uri::ToString()` form at entry because the
  upstream scan always compares `uri.toString()` strings.
- `SetMarkers` is upstream `updateMarker()`: identical content keeps `_nextIdx`;
  any change resets it to `-1`. `Selected` returns upstream's
  `MarkerCoordinate` (index, 1-based ordinal, total).
- **Divergence: no inline peek widget.** Upstream same-file navigation opens
  the `MarkerNavigationWidget` peek UI; this fork lands the caret at the marker
  start through `OpenDocumentAtMarkerPosition` → `MarkerPositionAdapter` and
  shows no peek. The widget, its title menu (the upstream surface for
  `editor.action.marker.next`/`prev`), and its `markersNavigationVisible`
  context key are unimplemented; those two commands therefore carry no Menu
  surface binding. The `*InFiles` pair's `goMenu` Menu-slot bindings are
  recorded metadata for parity — no native Go menubar exists.
- Session ownership (filter matching, the `reusePosition` step, doc-switch and
  caret-leave resets, landing) lives in `CEditWnd`, the
  `MarkerController` analog; see `window/CLAUDE.md`.

## Problems status-bar projection (`ProblemsStatusbarProjection.h`/`.cpp`, #382)

Pure value projection for the native `status.problems` (LEFT, priority 50),
`status.problemsVisibility` (LEFT, 49), and `statusbar.currentProblem`
(LEFT, 0) entries.

- `BuildProblemsStatusText` reproduces upstream `getMarkersText` verbatim:
  `"$(error) N $(warning) N"` always, `" $(info) N"` only when infos exist.
  `PackMarkerCount` is upstream `packNumber`: >9999 → the localized
  many-problems text ("10K+"), >999 → first digit + "K", else the number.
- `BuildProblemsStatusTooltip` is upstream `getMarkersTooltip`: only nonzero
  categories joined by `", "`; all-zero yields the localized "No Problems".
- `FindCurrentProblemStatus` is upstream
  `ShowCurrentMarkerInStatusbarContribution.getMarker`: gated on
  `problems.showCurrentInStatus`, resource-filtered to the active document,
  severity-sorted, first marker containing the caret; text is
  `"$(severity) <first message line>"`.
- Consumers are the native status bar (which stores the pushed text, parses
  `$(name)` codicons at paint, and owns hit targets/tooltips) and `CEditWnd`
  (which pushes the state on marker/caret/config changes). Neither becomes a
  marker authority.
- **Recorded payload divergence:** the `status.problemsVisibility` click
  delivers the canonical `["@id:problems.visibility"]` arguments JSON to
  `workbench.action.openSettings`, but the registered settings executor opens
  the native settings dialog and does not consume `@id:` queries — there is no
  settings-search surface to apply it to.
