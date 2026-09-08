# GitHub Actions

This independent SENP v2 extension contributes the upstream `github-actions`
container, `github-actions.current-branch` and `github-actions.workflows` Views.
All reads use the native repository broker. The guest has no token, process,
filesystem or network access.

Workflows remain visible even when disabled or without runs. Runs retain their
database identity and workflow identity; expanding a run pages its numbered
attempts, newest first. Each page contains at most 20 items. Attempt pagination
pins the first observed attempt count so a concurrent rerun cannot shift later
pages. Selecting a run or attempt opens metadata for that exact attempt.
Unknown states and a null conclusion remain explicit rather than implying success.

Current Branch requires exactly one verified repository with a nonempty branch
in the current workspace event. Detached, missing and ambiguous repositories
fail explicitly. Production workspace publication and package installation are
tracked in R01; the E05 native fixture proves Workflows through the real Wasm
host and native tree/document providers. Job/Step and log support follow in E06/E07.

Verify: `cargo test --locked --manifest-path rust/senp/Cargo.toml -p sakura-github-actions -p sakura-senp-github-client`
from the repository root. After building the Debug solution, run
`py -3 tools/verify-senp-runtime.py --offline` for the native composition path.
