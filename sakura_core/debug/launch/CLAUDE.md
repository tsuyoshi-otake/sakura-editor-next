# Phase 6 Launch Configuration Guidance

## Ownership

`CLaunchConfigurationCatalog` is a pure, bounded semantic catalog over one
resolved Launch artifact. It retains opaque adapter-specific fields but does
not interpret them as DAP messages, spawn an adapter, resolve variables, check
workspace trust, or own a debug session.

- Copy and validate the entire configuration/compound set before commit.
  Duplicate names, invalid compounds/references, bad schema, and capacity
  failures preserve the last-good catalog.
- Source generation and revision are monotonic fences. A missing source is
  cleared only by an explicit `Clear`; corrupt input is never treated as
  absence.
- Compound references must resolve inside the same accepted catalog. A later
  adapter may expand a compound, but it must not mutate catalog truth.
- Multi-root launch selection is folder-scoped. Never silently collapse it to
  the first folder or mix independent source revisions.
- `Stop` is terminal and prevents later Apply/Clear calls from reviving state.

## Verified Checkpoint

`LaunchConfigurationCatalog.*` passes 5/5. Adapter discovery, variable
substitution, trust checks, DAP process/session ownership, Run and Debug UI, and
compound execution are not implied by this catalog.
