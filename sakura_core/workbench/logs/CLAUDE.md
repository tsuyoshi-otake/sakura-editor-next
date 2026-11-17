# Session Logs Guidance (#421)

The session logs folder makes the Output Log channels and the current Problems
readable outside the window: by a user opening the file, and by an agent CLI
reading it from disk. It is diagnostic output, not durable state, so an editor
process writes it directly (as a VS Code window writes its own log files) and
never through the control-process storage.

## Layout

```
<Control profile home>\logs\
  20261115T064122-4242\          one folder per editor process (start time, PID)
    window1\
      problems.json              fork extension, see below
      Tasks.log                  a core.* owner's Log channel
      exthost\
        vscode.git\Git.log       the built-in Git integration
        <owner id>\<label>.log   any other owner's Log channel
```

- VS Code keeps `logs/<YYYYMMDDTHHMMSS>/window<N>/` under the user data
  directory. `<file name>.log` comes from the channel label, and extension
  channels sit under `exthost/<extension id>/`. The Git channel is upstream's
  `exthost/vscode.git/Git.log`, so the owner `workbench.scm.git` maps to
  `vscode.git`.
- The root is the **Control** profile's home, not the selected user-data
  profile's. Upstream's `logs/` sits beside the user data, not per profile.
- `LogSession` owns the names, the layout, and retention. `OutputLogFiles`
  owns the line format, the per-channel path, the writer, and the Output
  decorator. `ProblemsSnapshotFile` owns `problems.json`. `LogFileSystem` is
  the one I/O seam; tests inject a fake.

## Composition and lifetime

- `CNormalProcess` passes `WorkbenchRuntimeDependencies::sessionLogs` (root,
  file system, PID). Without a local profile path it passes nothing, and
  Output and Problems stay in memory only. Startup never fails because of
  logs.
- `CWorkbenchRuntime::StartSessionLogs` runs in `Start`, right after the
  Output provider exists and before any producer can append.
  1. It creates the window folder synchronously.
  2. It starts the writer. The retention sweep runs on the writer's worker
     before its first write, so startup does no deletion I/O. The sweep stops
     between deletions once Stop is requested.
  3. It wraps the provider in `CFileBackedOutputService`.
  4. It starts the `problems.json` writer. That writer does not depend on the
     Log writer, so a failure of one leaves the other running.

  Any failure sets a `SessionLogs` diagnostic (`logs.session`, `logs.output`,
  or `logs.problems`) and leaves that part off; the rest keeps working.
- `Output()` returns the decorator whenever it exists, so every borrower (the
  Output view, Git, the task observer) goes through it.
- `StopOwnedServices`:
  - After the provider stops, it quiesces the decorator. `Quiesce` takes the
    decorator lock, so an append the provider already accepted reaches the
    writer first, and nothing reaches it afterwards.
  - It then stops the Output writer, which writes what is queued and joins.
  - It then stops the `problems.json` writer before `MarkerService::Stop`, so
    the last file shows the markers as they were, not the emptied service.
  - The objects stay until the runtime is destroyed, so a borrowed pointer
    never dangles.
- A runtime-time write failure reports through the listener gate as a
  diagnostic and clears it when a later write succeeds. A failed write never
  fails the Output or Marker operation that produced it.

## Log files

- Line format: `YYYY-MM-DD HH:MM:SS.mmm [level] message`, followed by LF. The
  level is lowercase (`trace`, `debug`, `info`, `warning`, `error`).
- **Divergence:** upstream's logger stamps a line when it is logged. Here the
  decorator stamps it when the provider accepts it. The in-view projection is
  unchanged.
- Only a fresh `Succeeded` `AppendLog` reaches the file. A `Replayed` operation
  was already written once, and a rejected one never entered the channel.
  The file keeps the whole message even when the in-memory channel truncated
  it.
- Only Log channels are file-backed. Upstream also backs plain Output channels
  with files, but #421 scopes them out, so `openActiveLogOutputFile` is
  `NotApplicable` for them and the Output view's "..." menu disables Open in
  Editor.
- Clear Output empties the view, not the file, as upstream.
- File names:
  - Characters Windows forbids become `_`.
  - A name is cut to 96 characters and loses trailing dots and spaces.
  - A reserved device name gets a `_` prefix, including one with spaces
    before its extension (`NUL .x`), `CONIN$`/`CONOUT$`, and
    `COM`/`LPT` with a superscript digit.
  - A label ending in `.<digits>` has that dot replaced (`Foo.1` becomes
    `Foo_1.log`), so no channel's file is another channel's `.1.log` backup.
  - Two channels with the same label under one owner share a file.
  - An absolute path of 240 characters or more is passed in its
    extended-length (`\\?\`) form, because the process is not long-path
    aware. Open in Editor refuses a path of `_MAX_PATH` or more, which the
    legacy open cannot take.
- Bounds:
  - The writer queues at most 4 MiB. Beyond that it drops appends and counts
    them in `droppedBytes`.
  - A file that would grow past 5 MiB is moved to `<name>.1.log`, replacing
    the previous backup. A reader without delete sharing (`tail -f`, an
    editor) blocks that rename; the batch is then appended anyway, so the
    file grows past the bound instead of every later line being dropped.
  - A failed write drops its batch (counted in `failedWrites`) and forgets the
    folder, so the next write creates it again. The failing diagnostic
    changes once per batch, not per file.
  - **Divergence:** upstream's spdlog rotation keeps several backups. Here
    there is one.
- Retention: the newest 10 session folders, counting the current one.
  Anything that is not a `YYYYMMDDTHHMMSS-<pid>` name is never touched.
  - Every editor process is its own session, so a folder that is expired by
    age can still belong to a window that is open.
    `IsLogSessionProcessRunning` keeps a folder whose PID names a running
    process created no later than the folder's start time plus 2 s. A
    process created later has reused the PID, so its folder can be deleted.
  - A process that cannot be opened (access denied, for example an
    elevated editor) keeps its folder.
  **Divergence:** upstream's retention differs, and its folder name has no
  PID; the PID keeps two editor processes started in the same second apart.
  Each process has exactly one window, so the window folder is always
  `window1`.
- `workbench.action.openActiveLogOutputFile` opens the file as an ordinary
  document and reports `Succeeded` only when the file is open afterwards, in
  this or another window.
  **Known limit:** while the file is open as a Sakura document, the editor
  holds it without write sharing, so appends fail (counted in `failedWrites`,
  with the `logs.output` failing diagnostic) until that document is closed.
  **Divergence:** upstream opens a read-only output editor that
  follows the file. Here the document follows it only through the editor's
  normal file-change handling. Before the first line is written the file does
  not exist, and the command is `NotApplicable`.

## problems.json (fork extension)

VS Code has no file form of Problems. This file exists so a reader outside the
window, typically an agent CLI in the terminal, sees what the Problems view
shows.

- Shape: `{"version": 1, "revision": N, "markerCount": M, "truncated": bool,
  "markers": [...]}`. Each marker is the object the Problems view's Copy
  produces (`SerializeProblemsViewMarker`), in the view's order.
- Bounded at 10000 markers and 8 MiB. When cut, `markerCount` is still the
  total and `truncated` is true.
- A marker change marks the file dirty. The worker waits 200 ms, so a burst is
  one write, then replaces the file atomically (temp file, flush, rename). A
  reader never sees a partial file.
- A failed write is retried on its own, 500 ms first and doubling to 30 s,
  so a reader that briefly held the file without delete sharing does not
  leave it stale until some unrelated marker change. A successful write
  resets the delay.
- The parent folder is created again before each write.
- The marker listener holds a shared state object, never the writer, because
  `MarkerService::Unsubscribe` does not wait for a delivery that already
  copied the listener.
- Stop unsubscribes and joins without a final write. The last file stays after
  the window closed.

## Tests

`SessionLogsTest.cpp` covers:

- the line format and paths, including the forbidden and reserved names;
- session names and retention;
- the writer on a fake file system: order, Stop flush, rotation, a rotation
  a reader blocks, failing and recovery, one transition per batch, folder
  re-creation, the pending bound, and the before-first-write sweep;
- retention keeping a live session, giving up on Stop, and the liveness rule
  against this test process (live, reused PID, no such PID);
- the decorator: Log only, Succeeded only, replay, Clear, Dispose,
  DisposeOwner and Quiesce;
- the `problems.json` encoder and its truncation, the writer's coalescing,
  and the retry with no marker change;
- the real Win32 file system in a temporary folder.

`CWorkbenchRuntime.SessionLogsMirrorLogChannelsAndMarkersToTheSessionFolder`
covers the runtime composition end to end.
