# Workbench Integration Test Guidance

Integration scripts cover cross-process and real-backend gates for P0-P4. Each
run uses isolated profile/workspace roots, bounded timeouts/retries, and unique
artifact/process identifiers. Never use a developer's normal profile.

Verify multi-editor storage conflicts/resync, profile switching, dirty backup
recovery, terminal/task/debug/port teardown, and layout restoration as their
phases land. After every run, stop the scoped parent runner first, then children,
and re-list `tests1.exe` and `sakura.exe` to prove cleanup.

## Panel / first-editor layout regression (#292)

After `build-dev.bat x64 Debug`, run:

```powershell
pwsh -NoProfile -File src/test/integration/panel-editor-order.ps1 -Trials 3
```

The script launches visible windows in fresh profiles, compares both file-open /
Panel-maximize orders, asserts nonoverlapping document chrome and Panel headers,
and exercises restore. Maximization must hide the complete Editor Part; opening
the first file must restore the previous Panel height. Repeated editor snapshots
must retain explicit maximization. It records native geometry under `~/tmp/`, uses bounded
waits and commands, and checks editor/control exit and profile removal even on
failure. `-Executable` selects another built or installed application;
`-OutputDirectory` selects the evidence directory. It is independent of
`tests1.exe`; do not count it as a unit-test build or pixel verification.

The #293 extension opens the real Command Palette with Ctrl+Shift+P, selects
`workbench.action.toggleMaximizedPanel`, and checks both hidden-Panel reveal and
visible-Panel maximize. Restore through the native user-keybinding alias and
through the button must recover the same retained extent. This test requires
foreground access to its own disposable window and releases all injected keys.
Run it sequentially with other native/UI suites: even Explorer unit tests create
visible windows and can take focus between foreground selection and key delivery.
