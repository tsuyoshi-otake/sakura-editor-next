# Workbench Integration Test Guidance

Integration scripts cover cross-process and real-backend gates for P0-P4. Each
run uses isolated profile/workspace roots, bounded timeouts/retries, and unique
artifact/process identifiers. Never use a developer's normal profile.

Verify multi-editor storage conflicts/resync, profile switching, dirty backup
recovery, terminal/task/debug/port teardown, and layout restoration as their
phases land. After every run, stop the scoped parent runner first, then children,
and re-list `tests1.exe` and `sakura.exe` to prove cleanup.
