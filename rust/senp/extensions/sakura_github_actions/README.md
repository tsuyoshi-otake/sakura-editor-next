# GitHub Actions

This independent SENP v2 extension contributes the upstream `github-actions`
container, `github-actions.current-branch` and `github-actions.workflows` Views.
All reads use the native repository broker. The guest has no token, process,
filesystem or network access.

Workflows remain visible even when disabled or without runs. Runs retain their
database identity and workflow identity. As upstream, expanding a run lists the
jobs of its latest attempt followed, for a rerun, by "Previous attempts", which
lists every earlier attempt oldest first. The run row's ID carries the attempt
it was read at, so a concurrent rerun becomes a new row rather than shifting the
old row's children. Each page contains at most 20 items. Selecting a run or
attempt opens metadata for that exact attempt. Unknown states and a null
conclusion remain explicit rather than implying success.

Current Branch requires exactly one verified repository with a nonempty branch
in the current workspace event. Detached, missing and ambiguous repositories
fail explicitly. Production workspace publication and package installation are
tracked in R01; the E05 native fixture proves Workflows through the real Wasm
host and native tree/document providers.

Expanding an attempt reads its fixed `actions/runs/{run}/attempts/{attempt}/jobs`
path. Job IDs, not names, identify matrix rows; expanding a job pages its numbered
steps in order. A job summary includes runner metadata and a Step table. Missing
runner assignments and timestamps remain explicit. Details verify the job, run
and attempt together before publication, so a response for another rerun fails.
The native summary action uses `sakura.githubActions.openJobDetails`: upstream
provides log actions but has no corresponding native job-summary command.

A completed job's row carries upstream's inline "View job logs" action,
`github-actions.workflow.logs`, which receives the row's ID and opens the job's
log as a separate document. The log is downloaded by the `jobLog`
tool operation rather than by a repository path: the tool writes the bytes into
the editor's text resource store and answers with a handle, because an extension
has no effect for reading a resource and a log is far larger than one completion
may carry. The document names that handle only once the tool has answered with a
real one; a failed download publishes a document with no text section at all,
since the editor starts reading a text section as soon as it is published.

Verify: `cargo test --locked --manifest-path rust/senp/Cargo.toml -p sakura-github-actions -p sakura-senp-github-client`
from the repository root. After building the Debug solution, run
`py -3 tools/verify-senp-runtime.py --offline` for the native composition path.
