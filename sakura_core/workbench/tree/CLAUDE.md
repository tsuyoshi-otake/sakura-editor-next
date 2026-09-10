# Lazy native Tree Views

- `TreeViewModel` owns stable item IDs, tree snapshots, expansion, selection,
  pagination and limits without HWNDs or SENP dependencies. `SenpTreeProvider`
  owns the owner-scoped async subscriber. `CSenpTreeView` projects that state
  into a real native TreeView and never performs transport work in paint.
- Bounds are 2,000 retained items, depth 16, 256 items/page, 8 MiB of retained
  UTF-16 text/metadata, and eight concurrent parent loads. Validate an entire
  page before publication. Prepare changed nodes and siblings rather than
  copying the whole retained payload for every page. Reject duplicate,
  ancestor, reparented and malformed IDs; reject inconsistent page revisions
  and repeated cursors while preserving the last good snapshot.
- Root loads first. Leaves and collapsed/hidden branches never fetch. Pending
  parent loads deduplicate. Refresh preserves stable selection and user expansion;
  removed descendants and collapsed/hidden subscribers cancel explicitly.
  Failed, busy and timed-out loads require an explicit retry. Do not poll data.
- Demand raised while the runtime delivers results (`CanSubmit()` is false,
  i.e. inside the coordinator's drain) is not a failed admission. It stays
  queued in the model and the owner's post-drain provider Pump admits it.
  Submitting it anyway returned a bare `Unavailable`, so every extension-issued
  `InvalidateTree` - on activation and on each workspace change - failed the
  visible root until the user pressed Retry (#296).
- The native composition port mints owner-wide monotonic request generations.
  A derived tool-result event may have a different operation ID but retains its
  owner/workspace/account/request scope. Cancel unsubscribes all reads derived
  from that request; the broker retains physical cleanup ownership. Submission
  never pumps messages, reenters the provider, or waits for Wasm/tool work.
- One observer owns a body/provider pairing. Failed creation of another body
  must not close the existing observer's provider. Fatal native failure closes
  only an acquired provider, destroys the body, and calls `projectionFailed`
  to mark the containing cohort unusable. Close revokes both host callbacks.
- Native rows retain the real hierarchy, selection, keyboard navigation and
  MSAA/UIA ExpandCollapse behavior. Loading, empty, retry and next-page rows are
  typed host rows, never fabricated extension item identities. Native readable
  names combine label and description; full tooltip remains the native info tip.
  Unlike VS Code's tooltip-preferred aria name, the stock Win32 provider exposes
  the row text as its name. This is a documented accessibility mapping boundary.
- Commands execute on a matched down/up pair or Enter, never on selection.
  Command-bearing rows expand through their twistie; commandless rows use the
  requested single/double-click mode. Drag/drop, checkboxes and inline editing
  have no contract and remain unavailable.
- `TreeItem.icon` is a codicon name, empty, or a path. Upstream extensions pass
  `{light, dark}` file URIs inside their package; Sakura never reads package
  files for icons, so a path names one entry of the bundled, compiled-in
  vocabulary (today only the GitHub Actions run/job/step status icons, see
  `../icons/GITHUB-ACTIONS-ATTRIBUTION.md`). One theme-independent path stands
  for the upstream light/dark pair, and `ViewPaneChrome` picks the colours from
  the active colour theme kind. An unknown path draws no icon, never a
  substituted glyph. Upstream's in-progress spinner rotates; it is drawn at rest
  because a per-row animation timer is not allowed here (see the timer rule
  below).
- The bundled GitHub Actions extension follows upstream's run, job and step
  rows (name/number labels, state as icon, status and trigger in the tooltip),
  with two recorded divergences: tooltips are plain text without upstream's
  bold status, actor link and relative time (there is no Markdown tooltip
  contract, and the extension has no clock or timezone), and a job with no
  steps stays expandable because its Log row lives on its first child page
  until log opening becomes an inline row action.
- Intercept external `TVM_EXPAND` before the default procedure: after the first
  expansion Windows can omit its expansion notifications. Apply native updates
  only under the projection guard. Restoring the first visible row must ascend
  to a collapsed ancestor when the old anchor is hidden; otherwise Windows may
  silently expand that ancestor. Explicitly mask returned native state bits.
- Set `TVS_DISABLEDRAGDROP` because no drag provider exists. It also avoids an
  unnecessary nested drag-detection loop. Set `TVS_NONEVENHEIGHT` so 22 DIP is
  exactly 33 pixels at 144 DPI. Share the View/SCM palette, 22-DIP rows, 16-DIP
  icons and overlay scrollbar, and update the surface palette when moving Parts.
- A single native timer covers the earliest admitted load deadline and stops
  when no request remains. A posted, coalesced UI update handles changed
  branches. No idle timer, automatic error retry, or per-item polling is allowed.
  A package's `view/title` refresh action is the explicit trigger instead: it
  reaches the bound runtime as a no-argument command that invalidates its own
  View (#297, see `../senp/CLAUDE.md`).

Verify with a Debug solution build and
`TreeViewModel.*:SenpTreeProviderTest.*:SenpTreeView.*`, plus the owning View/page
and accessibility suites. Run `tools/verify-senp-view-rendering.ps1 -ProbeSet
TreeViews` separately for real-screen/PrintWindow/noise-floor capture. The
disabled visual fixture owns a 120-second deadline; the script owns and reaps
its exact PID. Native row text/state and first-visible changes prove gestures.

Upstream behavior references:
[VS Code Tree Views](https://code.visualstudio.com/api/extension-guides/tree-view),
[TVM_EXPAND](https://learn.microsoft.com/en-us/windows/win32/controls/tvm-expand),
[native styles](https://learn.microsoft.com/en-us/windows/win32/controls/tree-view-control-window-styles).
