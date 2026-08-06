# Update Guidance

## Scope

`sakura_core/update/` implements VS Code's update feature: the `updateState`
context key, the `update.*` commands, the title-bar Update indicator's backing
state, and the staged-installer/relaunch contract. It owns nothing about how
that state is drawn — the title bar is [`../window/CLAUDE.md`](../window/CLAUDE.md),
the commands are
[`../workbench/commands/CLAUDE.md`](../workbench/commands/CLAUDE.md), and the
`button.*` colors are [`../theme/CLAUDE.md`](../theme/CLAUDE.md).

Dependencies point one way: `window/` → `update/` → `platform/` + `config/`.
Nothing under `update/` may include a window, view, or workbench header.

## What is reproduced from VS Code exactly

These are upstream's identifiers and must not be renamed, re-cased, or
re-grouped:

- The twelve `StateType` values, published as the `updateState` context key's raw
  string — spaces included, so `"checking for updates"` is the literal value a
  `when` clause compares against. Default `uninitialized`
  (`platform/update/common/update.ts`, `contrib/update/browser/update.ts`).
- The gear-menu group `7_update` and all eight entries with upstream's titles:
  `update.check` "Check for Updates...", `update.checking`
  "Checking for Updates...", `update.downloadNow` "Download Update (1)",
  `update.downloading` "Downloading Update...", `update.install`
  "Install Update... (1)", `update.updating` "Installing Update...",
  `update.cancelling` "Cancelling Update...", `update.restart`
  "Restart to Update (1)". The four `...ing` entries are contributed with
  `precondition: false` upstream and are therefore disabled here, not omitted.
- The title-bar entry `workbench.actions.updateIndicator`, title `"Update"`,
  `MenuId.TitleBarUpdate` order 0, visible only for the three actionable states
  `available for download` / `downloaded` / `ready`, and only while
  `update.titleBar` is true (`contrib/update/browser/updateTitleBarEntry.ts`).
- The palette commands `update.checkForUpdate`, `update.downloadUpdate`,
  `update.installUpdate`, `update.restartToUpdate`, `update.showUpdateInfo`.
- The settings `update.mode` (`none|manual|start|default`, default `default`),
  `update.enableWindowsBackgroundUpdates`, `update.showReleaseNotes`,
  `update.titleBar`.
- The transitions themselves, including the `Archive` path that offers the
  release page rather than a self-install.

## Divergences

Each entry below is a place this product cannot or does not do what upstream
does, with the reason. An undocumented divergence is a bug.

### The feed is this fork's GitHub Releases, and only stable releases count

Upstream talks to Microsoft's update service, which has no counterpart here. The
feed is `https://api.github.com/repos/<owner>/<repo>/releases/latest`, derived
from the `GIT_REMOTE_ORIGIN_URL` recorded in the generated `githash.h`, so a
fork checks its own releases and a build with no recorded origin checks nothing.

`releases/latest` **is** the stable-only decision: GitHub excludes drafts and
pre-releases from that endpoint, and the array parser applies the same
`draft == false && prerelease == false` filter, so the rule holds on either
shape. **Consequence, stated because it looks like a defect: every release
published in this repository so far is a Pre-release, so the Update indicator
will not appear until a stable release exists.** That is the intended
fail-closed behavior, not a broken check.

An asset that does not match this build's architecture, a release with no
matching asset, a size mismatch, a digest mismatch, and a release that publishes
no usable digest at all resolve to "no update" plus a diagnostic. There is
deliberately no "close enough" branch: the alternative is running an unverified
installer against the user's installation.

The last of those is worth naming, because the size check looks like it could
stand in for the digest and cannot. A size proves only that the expected number
of bytes arrived; the next thing `VerifyPayload` returning true causes is that
file being run as an installer over the user's installation. `ParseAssetDigest`
yields no digest for an absent field *and* for one that is present but not a
well-formed `sha256:<64 lowercase hex>`, so both shapes take the same refusal
rather than degrading to the size check. Every asset in this repository's real
release feed carries a `sha256:` digest today, so this is a fail-closed guard
against a future feed change, not a branch that fires in normal operation.

The digest is GitHub's own published value for the asset, which makes this an
integrity check against a corrupted or truncated download and against a feed
that disagrees with its own asset — **not** an authenticity check. Nothing here
verifies an Authenticode signature on the downloaded installer, so the trust
root is the TLS connection to `api.github.com` and `github.com` plus whatever
Windows itself enforces when the installer is launched. Do not describe the
digest check as proving the installer's publisher.

### One service per window, converging through the staging directory

Upstream runs a single update service in the main process and broadcasts to
every window. This process model has no such owner — the control process has no
workbench, and each editor window is a separate process. Each runtime-backed
window therefore composes its own `UpdateComposition`, and the shared truth is
the staging directory: `Initialize()` derives the opening state from the
manifest and the installer on disk, so a second window opened after a download
starts at `ready` rather than re-downloading.

Two windows told to download at the same moment will both download. The
plan called for a user-scoped named mutex around the download and that is **not
implemented**; the cost is duplicated bytes and one wasted write, not a
corrupted stage, because `StoreInstaller` never reports a partially written file
and the digest is verified before the manifest is written. Add the mutex if
duplicate downloads ever become observable.

### `update.mode`'s four values are an observed scheduling difference, not just a gate

`AllowsAutomaticCheck` and `AllowsPeriodicCheck` (`UpdateService.h`/`.cpp`) are
policy *decisions*; something has to act on them or `start`, `default`, and
`manual` are indistinguishable except by whether the user happens to click. That
something is `IUpdateScheduler` (`IUpdateService.h`) and its production
implementation `UpdateAutoCheckTimer` (`UpdateAutoCheckTimer.h`/`.cpp`): a
single-slot, generation-counter delayed-callback timer modeled directly on
`UpdateWorkerExecutor` for the same cancel/replace/join correctness. It is a
distinct seam from `IUpdateExecutor::Post` — that one runs work now, on the
worker thread; `IUpdateScheduler::PostDelayed` runs it later, and only the most
recently posted item survives.

`UpdateService::Impl` arms it in exactly two situations, both named for what
they mean rather than for the code path that reaches them:

- `PublishIdleAndScheduleInitialCheck` — used only by `Initialize()`'s three
  ways of landing at `idle` (no manifest, a recorded failure, a stale/foreign
  manifest). Arms the one-time `options.initialCheckDelay` (default 30s,
  matching upstream's own startup delay) when `AllowsAutomaticCheck(mode)` is
  true, i.e. `start` or `default`.
- `PublishIdleAndSchedulePeriodicCheck` — used everywhere a check, download, or
  apply cycle (or its cancellation) settles back at `idle`: every failure branch
  of `RunCheck`/`RunDownload`/`RunApply`, `ObserveCancellation`, and the failure
  branches of `QuitAndInstall`. Arms the recurring `options.periodicCheckInterval`
  (default 1h) only when `AllowsPeriodicCheck(mode)` is true, i.e. `default`
  only. `start` reaches `idle` through these same paths but never rearms itself
  here — only its one `Initialize()`-time timer ever fires.

The timer's own callback re-enters through `Impl::TriggerCheck(explicitRequest
= false)`, the same method `UpdateService::CheckForUpdates` calls for the
user's own click (`explicitRequest = true`). A tick that finds the state
machine busy with something else (not `idle`/`uninitialized`) does not silently
disappear: it calls `ScheduleNext(initial = false)` itself before returning, so
`default`'s polling cannot permanently stop just because it raced an explicit
check. `mode == none` is refused unconditionally at the top of `TriggerCheck`,
before the busy check, matching `Initialize()` never arming anything for `none`
in the first place.

`Impl` holds a `std::weak_ptr<Impl> self`, set by `UpdateService`'s constructor
right after the owning `shared_ptr` exists, so a timer callback can safely
reach back into `Impl` without a raw `this` capture that would dangle if the
service were destroyed while the timer was still armed — the same reasoning
`UpdateService`'s public methods already apply to posted executor work.

`UpdateServiceDependencies::scheduler` is therefore mandatory, checked by
`IsComplete()` alongside the other five collaborators; a composition that
forgets to wire it fails closed to `Disabled`, not to a silently-inert `start`/
`default`. `UpdateComposition::Impl` owns one `UpdateAutoCheckTimer` and stops
it before `executor.Stop()` in `Shutdown()` — a tick already in flight posts to
the executor, and stopping the executor first would only make that post go
nowhere rather than never happen.

Tests exercise this through a `FakeScheduler` (`UpdateServiceTest.cpp`) that
records the one pending `{delay, work}` item instead of waiting on a real
clock, with a `Fire()` helper the test calls explicitly. No test in this
repository waits on `std::chrono::steady_clock`.

### The configuration snapshot is frozen per window

`update.mode`, `update.enableWindowsBackgroundUpdates`, and `update.titleBar`
are read once, as one coherent `ReadSnapshot` (never repeated `GetValue` calls —
see [`../config/CLAUDE.md`](../config/CLAUDE.md)), when the window's stack is
composed. Changing them takes effect in the next window, matching the way
`CreateOpenVsxProductionClient` freezes its own network policy.

`update.showReleaseNotes` is registered with upstream's id, type, and default,
but nothing consumes it yet: there is no release-notes editor input to show.
`update.showUpdateInfo` opens the release page in the browser instead. The
setting is present so a user's existing `settings.json` keeps validating, not
because the behavior it names exists.

`http.proxyStrictSSL = false` is refused outright at composition. The production
WinHTTP transport has no TLS-validation escape hatch, so honoring that setting
would mean downloading an installer over an unvalidated connection.

### A non-installed build is `Archive`, and is never given a fake install

`IUpdateInstallLocation` resolves Inno's uninstall entry for AppId
`sakura editor` (`installer/sakura-common.iss`) and requires the running
executable to actually live under the recorded `InstallLocation`. A developer
build, or a copy unzipped somewhere else, resolves to `Archive`: the state
machine stops at `available for download` and `update.downloadNow` opens the
release page. This matches upstream's own archive behavior and satisfies the
repository rule against faking a capability — an installation that cannot
replace itself is sent to the release, not shown a self-install that would
overwrite an unrelated directory.

### The restart contract, and the two Inno switches that were dropped

"Press Update, restart, come back" is implemented as an explicit hand-off, not
as a dependency on Restart Manager:

1. `update.restart` sets `applyOnExit` in the staging manifest.
2. The workbench runs the ordinary `workbench.action.quit`, so save prompts
   behave exactly as they always do.
3. `CControlProcess::OnExitProcess` — the last process of the application to
   exit — calls `RunPendingUpdate`, which starts the staged installer detached
   and exits.
4. Setup runs with
   `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="<InstallLocation>" /UPDATERELAUNCH=1 /LOG="<staging>\install.log"`
   and its `[Run]` entry relaunches `{app}\sakura.exe` when
   `ShouldRelaunchAfterUpdate` sees `/UPDATERELAUNCH=1`.

Two switches from the original design were removed after reading the package:

- **`/CURRENTUSER` is not passed.** `sakura-common.iss` sets
  `PrivilegesRequired=lowest` with no `PrivilegesRequiredOverridesAllowed`, so
  Setup rejects the switch outright and the whole install would fail.
- **`/NOICONS` is not passed.** In a reinstall it does not mean "leave the Start
  Menu alone"; it means "install no icons", which removes the shortcuts the user
  already has. Updating must not silently delete them.

The relaunch entry is a **second** `[Run]` line, without `postinstall` and
without `skipifsilent`. The existing interactive "start now" entry has both, so
exactly one of the two ever fires: the interactive one in a normal install, the
relaunch one in the silent update install.

`InitializeSetup` normally loops on `CheckForMutexes('MutexSakuraEditor')`,
showing a custom retry form. `/SUPPRESSMSGBOXES` does not suppress a custom
form, so under an update install that would block forever on a window nobody can
see. `ConfirmApplicationIsClosed` therefore takes a different branch when
`ShouldRelaunchAfterUpdate` is true: it polls the mutex for up to 30 seconds and
aborts if the editor really is still running. The installer is started from
`OnExitProcess`, before the control process's destructor releases that mutex, so
a short wait is the normal case rather than an error.

A launch failure disarms `applyOnExit` and records the reason in the manifest,
so the next session explains itself instead of retrying the same failing launch
on every exit. A successful launch deliberately leaves the manifest armed: the
installer replaces this installation, and it is the next session's `Initialize`
seeing a version that is no longer newer that clears it.

### Cancelling the restart is detected through the state gate, not through `this`

`workbench.action.quit` reaches `CControlTray::TerminateApplication` and runs
**synchronously**; it can destroy `CEditWnd` before it returns. So
`CEditWnd::ExecuteUpdateQuitAndInstall` may not touch `this` after the quit
unconditionally. It holds a `std::weak_ptr` to the window's update state gate —
the one piece of that window's state that outlives the window — and
`CloseUpdateProjection` clears the gate's `connected` flag during teardown. A
gate that is still connected after the quit returns is therefore proof that the
window survived, which can only mean the user cancelled; only that path calls
`AbortQuitAndInstall()`. Do not "simplify" this into a plain `this` capture.

### Verified scope

**Applying an update on real hardware has not been verified.** The agreed
verification for this feature is unit tests only: the feed parser, the version
order, every state transition, and the installer command-line string are tested,
and nothing in the test suite touches the network, the registry, or a real
installer. What is *not* verified is the end-to-end run — a published stable
release, a real download, Setup replacing a live installation, and the relaunch
coming back. Do not describe that path as working; describe it as implemented
and untested until someone runs it.
