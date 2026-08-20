# Phase 6 Tasks Guidance

## Configuration Ownership

`CTaskConfigurationCatalog` is a pure semantic catalog over one resolved
workspace-artifact Tasks document. It owns no file reads, workspace selection,
terminal, process, problem matcher, or native panel.

- Preserve source URI, generation, and revision from the accepted artifact.
  Reject stale generations/revisions without replacing the last-good catalog.
- Validate and copy a complete bounded catalog before commit. Duplicate labels,
  invalid entries, and oversized input leave the previous catalog intact.
- Shell and Process tasks may be marked runnable only when their unsupported
  capability flags are empty. Custom execution, dependencies, background
  tracking, and problem matchers remain explicit unsupported capabilities until
  their owners exist.
- Multi-root resolution is folder-scoped. Do not silently select the first
  folder or put every folder into one global catalog.

## Execution Boundary

Task execution composes a selected immutable definition with a terminal-session
factory. It must use explicit run IDs, bounded concurrency, replay/deduplication,
and one terminal result for spawn failure, exit, cancellation, forced close,
host loss, and shutdown. Shell policy belongs behind an injectable boundary;
never create a command line by concatenating untrusted arguments.

`ITaskExecutionSession` deliberately splits nonblocking, idempotent
`BeginClose` from `WaitForClose(absoluteDeadline)`. Every wait result, including
timeout, promises that the host and workers are quiescent. Close fanout must use
one shared absolute deadline, fence in-flight startup, and never turn a callback
into an accidental terminal state. A callback-originated service Stop returns
`Deferred`; the safe outer Start/Cancel/Close/completion boundary owns final
Stop publication. External and repeated Stop callers observe the same terminal
result.

Hiding or destroying a panel does not cancel a task. Problem matcher output must
enter `MarkerService` through its own generation/revision adapter, and task
output must enter the terminal or `OutputService` owner rather than a view-local
buffer.

## Problem Matcher Translation

`ProblemMatcherEngine` (`ProblemMatcherEngine.h/.cpp`) is the pure VS Code
`problemMatcher`-shaped translator from captured task output lines to
`workbench::problems::ReplaceMarkersRequest` values. It is a stateless
function-only class, not a service: it owns no file reads, process, HWND,
timer, or thread, and every unsupported matcher shape (an unresolvable
`$name`, `fileLocation: autoDetect`/`search`, an out-of-range capture group, a
non-numeric line/column, an unrecognized severity spelling, malformed UTF-8
output) is a typed terminal `EProblemMatchStatus` rather than a guessed
default or a silently dropped line.

- `BuiltinProblemMatchers::Resolve` covers `$msCompile`, `$gcc`, and `$tsc` by
  exact `$name`. There is no fuzzy or partial-name resolution.
- Multi-pattern (VS Code "multiline") matchers are a small state machine over
  the pattern chain: a completed non-`loop` chain resets to pattern 0; a
  `loop` last pattern keeps its accumulated fields (notably `file`) and keeps
  matching itself, so repeated detail lines under one unchanged header each
  produce a marker; a line that breaks the chain before its last pattern
  resets state and retries that same line against pattern 0 rather than
  silently discarding it.
  `Relative` resolution requires a caller-supplied workspace-root URI and does
  no filesystem access; `Absolute` requires the captured text to already be a
  well-formed absolute Windows/UNC path. Both go through
  `platform::uri::Uri::FromWindowsPath`, never a manual string-to-URI shortcut.
- Recorded divergence: VS Code extends a bare `endColumn` to the end of the
  matched source line by consulting the live document. This adapter has no
  document access, so a pattern that supplies `column` but not `endColumn`
  produces a minimal one-character range (`endColumn = column + 1`) instead.
- This class only translates already-captured text into requests it hands
  back to the caller; it does not call `MarkerService` itself, does not read
  `TaskConfigurationCatalog::problemMatchers` names, and is not yet wired to
  `TaskExecutionService`'s output path. Resolving a task's configured
  `problemMatchers` string(s) into a `ProblemMatcherDefinition` (built-in
  lookup vs. a workspace-defined matcher) and feeding live task output through
  this engine into the runtime-owned `MarkerService` remain the composition
  adapter's job, not this pure class's.

## Verified Checkpoint

`CFolderTaskCatalogRegistry` owns one explicit slot per canonical workspace
folder. It copies all selected Tasks artifacts and service generation/state
through one atomic batch read, applies folder override or workspace fallback per
folder, treats folder reorder as an identity no-op, and never invents a default
or chooses the first folder. A typed catalog rejection retains that slot's
last-good value and remains catalog-local; JSON/schema diagnostics stay with the
artifact source rather than racing global runtime readiness.

`CWorkbenchRuntime` owns the registry and `TaskExecutionService`, exposes them
only while Ready, reconciles known-empty/add/remove/reorder topology, disconnects
artifact notifications before shutdown, and will not publish `Stopped` until
Task Stop has completed. A Task listener that reentrantly requests runtime Stop
observes `Busy`; Task's safe outer boundary completes deferred close/join, and
an external Stop owns final runtime publication.

`CTaskTerminalSessionFactory` is compiled into production composition. Process
launch preserves executable/argv/cwd/size, shell launch uses an injected
PowerShell policy that quotes each argument token, and completion is mapped once
from `CTerminalSession` only after the root process, job descendants, backend,
and workers are quiescent. The deterministic real ConPTY smoke verifies a
nonzero root exit code instead of substituting EOF or a fabricated zero.

The focused checkpoint covers `TaskConfigurationCatalog`,
`FolderTaskCatalogRegistry`, `TaskExecutionService`, the production adapter,
real ConPTY integration, runtime catalog/lifecycle integration, and Terminal
session lifecycle. Keep its exact current count in the goal-loop journal rather
than copying a stale number here.

Variable/environment resolution, dependency/background scheduling, provider/
custom executions, presentation policies, and extension Task RPC remain
incomplete. `ProblemMatcherEngine` gives problem-matcher text translation a
pure, tested home, but it is not yet composed into the production Task
adapter, so a running task's output does not yet reach `MarkerService`.
Production currently constructs the adapter without a
Task-output presentation sink; bytes are bounded and drained but not visible in
the native Terminal panel. The future projection must share a runtime-owned
terminal model/session authority with the panel rather than treating
`OutputService` or a HWND-local buffer as a substitute for VS Code's Task
Terminal.
