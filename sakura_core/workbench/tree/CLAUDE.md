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
- A page's `revision` is the provider's version of ONE parent's listing: the
  first page and every page appended through its cursors carry the same value,
  and a later listing (refresh or invalidation) may keep or raise it. The host
  rejects a lower value as Stale, and an appended page whose value differs
  from the stored one ("the tree changed while this page was loading").
  Providers must not derive it from the page number or the workspace revision.
- On `TreeLoadKind::NextPage` only, an item whose id is already a retained
  DIRECT child of the same parent is dropped rather than rejected: offset
  paging repeats the boundary row when an item is created or closed between
  the two fetches. The retained node keeps its position, payload, subtree,
  expansion and selection, and dropped items count toward neither item nor
  byte totals. Still `Invalid`, atomically: an id repeated inside one page,
  an appended id retained under another parent, an ancestor/cyclic id.
  Initial/Refresh keep full replacement semantics. An offset shift in the
  other direction (a skipped item) is undetectable; Refresh restores the
  full list (#351).
- Root loads first. Leaves and collapsed/hidden branches never fetch. Pending
  parent loads deduplicate. Refresh preserves stable selection and user expansion;
  removed descendants and collapsed/hidden subscribers cancel explicitly.
  Failed, busy and timed-out loads require an explicit retry. Do not poll data.
  Host failure reasons are typed: the model and provider record a
  `TreeFailureReason` on the snapshot, and `CSenpTreeView` localizes it at the
  row through the owner-installed `failureTexts` table (built-in English
  fallback otherwise). A provider page message stays verbatim, except an
  exact executor/broker token (`tool-unavailable`, `rate-limited`,
  `page-too-large`, `denied`, the control-broker sentences, ...) resolved
  through the view's `tokenTexts` exact-match table; wire tokens are unchanged.
  The model owns the reason's meaning: `ProviderMessage` is recorded only with
  non-empty text (an empty, invalid or over-budget message becomes
  `LoadFailed`), and `Begin` clears a recorded failure.
  The token set lives only in `SenpTreeView.cpp` (`kTokenGroups` maps each
  wire token to a `TreeTokenText` display group); owners supply localized
  text per display group, never a token list.
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
  vocabulary (today the GitHub Actions run/job/step status icons and its
  `logs.svg` action icon, see `../icons/GITHUB-ACTIONS-ATTRIBUTION.md`). One theme-independent path stands
  for the upstream light/dark pair, and `ViewPaneChrome` picks the colours from
  the active colour theme kind. An unknown path draws no icon, never a
  substituted glyph. Upstream's in-progress spinner rotates; it is drawn at rest
  because a per-row animation timer is not allowed here (see the timer rule
  below).
- The bundled GitHub Actions extension follows upstream's run, job and step
  rows (name/number labels, state as icon, status and trigger in the tooltip),
  with two recorded divergences: tooltips are plain text without upstream's
  bold status, actor link and relative time (there is no Markdown tooltip
  contract, and the extension has no clock or timezone). A job with no steps
  is a leaf, as upstream.
- Inline row actions follow VS Code's `view/item/context` `inline` group
  (#297). `SenpTreeItemAction` matches the row's `TreeItem.contextValue`
  (every `contains` token and every `equals` value); a row with no matching
  action draws none. The actions are drawn right-aligned on the row and a click
  selects the row and runs the command with the row's stable ID as its only
  argument, SENP's stand-in for the element VS Code passes. A hidden or
  non-current row refuses the action. At most eight actions per View.
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
  View (#297, see `../senp/CLAUDE.md`). A push is the other trigger: the
  published workspace snapshot carries the SCM ahead count, and a drop
  invalidates the Actions extension's CURRENT BRANCH view, as upstream does.

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
