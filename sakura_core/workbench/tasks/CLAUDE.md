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
  capability flags are empty. Custom execution, dependencies, and background
  tracking remain explicit unsupported capabilities until their owners exist.
  Problem matchers are supported only for the exact `$msCompile`, `$gcc`, and
  `$tsc` built-ins that `BuiltinProblemMatchers::Resolve` recognizes; any other
  matcher name or an inline matcher object stays a typed unsupported flag, so
  the task remains non-runnable rather than silently dropping the matcher.
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
  back to the caller; it does not call `MarkerService` itself and does not
  read `TaskConfigurationCatalog::problemMatchers` names. Resolving a task's
  configured `problemMatchers` string(s) into a `ProblemMatcherDefinition`
  and feeding live task output through this engine into the runtime-owned
  `MarkerService` is the composition adapter's job — that adapter now exists
  as `TaskProblemCollector` below.

## Tasks → Problems Producer (`TaskProblemCollector.h/.cpp`, #380)

`TaskExecutionService` accepts an optional `TaskRunObserverFactory`. For every
accepted run it asks the factory for an `ITaskRunObserver` and delivers that
run's drained output bytes plus its exactly-once terminal state. Both terminal
session factories forward the raw stream, and the runtime-backed adapter
flushes the final drained bytes before publishing completion, so the observer
sees the same byte stream the terminal VT parser consumed.

`MakeTaskRunObserverFactory(markers, output, book)` is the production
composition: it declines tasks without a `BuiltinProblemMatchers`-resolvable
`$name`, reports an unresolvable matcher name to the Tasks Output channel, and
otherwise returns an observer that buffers bounded output (4 MiB cap), splits
it into LF-separated UTF-8 lines at the run's terminal transition, invokes
`ProblemMatcherEngine`, and applies each `ReplaceMarkersRequest` through
`MarkerService::Replace` under the matcher owner and the run's generation.

- `CTaskMarkerGenerationBook` mints each run's marker-owner generation at
  observer creation, keyed by a stable owner ID derived from the task source
  URI and label. A delayed terminal callback from an older run therefore can
  never mint a generation newer than an in-flight run's, and
  `MarkerService::Replace`'s own generation fencing rejects any stale write
  that still slips through.
- The book also keeps the last-delivered resource set per owner. On a
  terminal transition the collector computes `changeOne`-equivalent
  replacements for newly reported resources and `remove`-equivalent empty
  replacements for resources the new run no longer reports, matching upstream
  `problemCollectors.ts` semantics: a rerun with zero matches clears the
  prior run's markers rather than leaving them stranded.
- A task with no matchers gets no observer and costs nothing. Matcher-engine
  typed failures (`InvalidUtf8Line`, capacity, and friends) and
  `MarkerService` failures are appended to the `tasks` Output channel as
  diagnostics; they never change the run's terminal result.
- `EnsureTasksOutputChannel`/`AppendTasksOutputLine` own the core `tasks`
  channel (`ownerId: core.tasks`, generation 1, `Output` kind, label
  `Tasks`). The channel is created idempotently and carries task-system
  messages only — configuration errors, unknown matcher names, collector
  failures, and command diagnostics. Task process output never enters this
  channel; it stays in the terminal instance.
- Workspace-root resolution derives the folder URI from a `/.vscode/tasks.json`
  source URI. Workspace-file tasks without a folder root get the typed
  `MissingWorkspaceRoot` failure for `Relative` matchers, matching upstream's
  treatment of an unresolvable `${workspaceFolder}`.

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
incomplete. The Tasks → Problems producer above is composed in production:
`CWorkbenchRuntime` injects the collector factory into `TaskExecutionService`
through a lazy output-provider lookup (the provider is composed after the
service, and no run can start before Ready publication). A running task's
output reaches `MarkerService` through `TaskProblemCollector`, so the Problems
view now has a real producer for `$msCompile`/`$gcc`/`$tsc` matchers.

Task terminals run as real `Task`-origin `TerminalInstance`s in the shared
`CTerminalRuntimeService`: drained bytes feed the instance's live VT model and
are also copied into `TerminalInstanceDrainResult::drainedOutput` for the
observer, so output lands in the runtime-owned terminal authority rather than
a discarded drain or a view-local buffer. Recorded divergence: the native
panel's `TerminalTabManager` only projects `Interactive`-origin topology
sessions, so a Task terminal is not yet visible as a panel tab the way
VS Code reveals its task terminal. Surfacing Task instances as panel tabs is
a future projection; it must keep the same runtime-owned instance authority
rather than duplicating a HWND-local buffer.
