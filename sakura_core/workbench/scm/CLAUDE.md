# Source Control Guidance

## Scope

This directory owns the SCM authority (`SourceControlService`), the built-in Git
repository reader (`GitCommandRunner`, `GitScmModel`), the bridge that publishes
that repository into the SCM model (`GitScmPublisher`), and the native Source
Control view (`CScmWorkbenchTool`).

The tracking Issue for the current work is #19.

## One SCM Authority, No Second Truth

`SourceControlService` is the only authority for what the Source Control view
shows. The built-in Git repository is published through the same provider-neutral
service boundary used by every SCM consumer, under VS Code's
own `vscode.git` / `git` identities, so a consumer that already knows VS Code's
SCM model finds the provider it expects.

- `GitScmState` is a **parse result**, never a render source. `CScmWorkbenchTool`
  builds its rows from published `ScmProviderState` values only. A change to the
  view must not reintroduce a path that reads `GitScmState` directly, because two
  render paths drift and the branch and the file list would then be able to
  disagree.
- One `CreateProvider` call carries the provider and all four groups, and the
  service applies it as one revision. Do not replace it with an update followed
  by per-group `ReplaceResources` calls: that reopens a window in which the
  branch has already changed but the file list still belongs to the previous one.
- A publisher that has not changed its (root, state, policy) publishes nothing,
  so an idle 5-second refresh costs no service revision.
- A successfully read `state.repository == false` **retracts** the provider.
  A failed `git status` is not a replacement snapshot: it retains the last
  successfully published provider, input, resource groups, decorations, and
  Graph history while recording the execution diagnostic. Changing the semantic
  workspace root explicitly retracts the old provider before probing the new
  root, so retention can never leak one repository's state into another.
- A window with no `CWorkbenchRuntime` has no service to borrow. It builds the
  same publication locally through `BuildGitPublication` and renders that, rather
  than growing a second row-shaping path for that configuration.

## Verified VS Code Behavior Reproduced Here

Verified against `microsoft/vscode` sources, not inferred from screenshots.

- Groups and labels, in upstream declaration order: `merge`/`Merge Changes`,
  `index`/`Staged Changes`, `workingTree`/`Changes`,
  `untracked`/`Untracked Changes`.
- `hideWhenEmpty` is `true` for `merge` and `untracked` (explicit upstream
  assignments) and for `index` (upstream drives it from
  `git.alwaysShowStagedChangesResourceGroup`, default `false`). `workingTree`
  has no assignment, so it stays `false` and its header shows while empty.
- The view filter is upstream's `SCMTreeFilter`, verbatim:
  `resources.length > 0 || !hideWhenEmpty`. `IsVisibleGroup` in
  `CScmWorkbenchTool.cpp` is that condition and nothing else.
- Git has two areas, so one path with both staged and unstaged edits appears in
  **both** Staged Changes and Changes. `GitResourceGroupSet` is a set, not an
  enum, for exactly this reason, and the provider `count` counts resources, not
  files.
- A conflicted path belongs to Merge Changes alone. Listing it under Changes as
  well would offer a stage action for a file that must be resolved first.
- Status text (`Index Modified`, `Conflict: Both Added`, …) and the
  strike-through rule for every deleted status follow upstream's own strings.
- The status bar encodes dirtiness in the **icon** — `$(git-branch)`,
  `$(git-branch-changes)`, `$(git-branch-staged-changes)`,
  `$(git-branch-conflicts)`, `$(git-commit)` when detached — **and** in the
  label's trailing markers. **Corrected record:** an earlier version of this
  section said the markers must not be appended because upstream does not append
  them. That was wrong. Upstream's `headLabel` is `headShortName` plus `*` for
  working-tree or untracked changes, `+` for staged changes, and `!` for a merge
  or rebase in progress, in that order, and its status item renders icon and
  label together. `GitHeadLabel` reproduces exactly that; the icon is not a
  substitute for the markers, nor the markers for the icon.
- `headShortName` is the branch name, else the object name cut by upstream's
  literal `substr(0, 8)`. That 8 is **not** `git.commitShortHashLength`, whose
  default is 7 and which governs only the checkout Quick Pick's object-name
  descriptions. The two lengths differ in upstream, and `kGitCommitShortHashLength`
  must not be substituted here: doing so would put this status item one character
  off a real VS Code. `headShortName` is empty only when HEAD names nothing at
  all — a detached HEAD with no commit — where upstream's callers take their
  no-branch path. The status item names `HEAD` there rather than rendering a
  blank, unclickable gap.
- The commit input box placeholder is resolved through the publication text
	keys `GitCommitMessage` / `GitCommitMessageOnBranch`, so the active language
	controls both the wording and the shortcut label. The English fallback is
	`Message (Ctrl+Enter to commit on "<headShortName>")`, with the branch in
	double quotes, and drops the `on "…"` clause entirely when there is no short
	name. The `{0}` argument is the branch or detached-head short name; the
	shortcut text is part of the localized resource because this native editor
	does not have Monaco's keybinding formatter.
- The status items carry upstream's own tooltips: `<headLabel>, Checkout
  Branch/Tag...` for the branch item, and `Publish Branch`,
  `Synchronize Changes`, `Pull N commits from <upstream>`,
  `Push N commits to <upstream>`, or `Pull N and push M commits between
  <upstream>` for the sync item.
- The sync item is `git.publish` / `$(cloud-upload) Publish Branch` with no
  upstream branch, otherwise `git.sync` / `$(sync)` with `N↓ M↑` appended only
  when the branch has actually diverged.
- The status letter (`M`, `A`, `D`, `R`, `C`, `U`, `!`) is **not** part of
  `SourceControlResourceState`; upstream publishes it through a separate
  FileDecorationProvider. `GitResourceDecoration` is a side table keyed by
  `Uri::ToString()` for the same reason. A provider we did not publish therefore
  has no badge of ours, and the view renders none rather than inventing one.
- One file has one badge even when it occupies two rows, because that table is
  keyed by URI. Upstream fills its map in the order index, untracked,
  workingTree, merge, and a later group overwrites an earlier one, so the
  effective precedence is merge > untracked > workingTree > index and
  `GitStatusLetter` reproduces exactly that. Do not "simplify" it to always read
  the index column: a staged add that was edited again must show `M`, not `A`.
  Provider `count` counts resources and the decoration table counts files, so
  the two numbers legitimately differ and must not be conflated.

## SCM View Stack

`workbench.view.scm` remains the `Source Control` ViewContainer.  Current VS
Code places three sibling Views in that container, rather than putting a single
`Source Control` title inside the change list:

| View | Stable ID | Native presentation |
| --- | --- | --- |
| Repositories | `workbench.scm.repositories` | Header plus the published repository row |
| Changes | `workbench.scm` | Header, input box, resource groups, or welcome content |
| Graph | `workbench.scm.history` | Header plus the owner-drawn history list |

`CViewContainerPages` owns one physical `CScmWorkbenchTool` HWND for the
container, so the tool projects those Views into a vertical stack.  It does not
create an Activity Bar item, a second side-bar Part, or a separate window for
Graph.  The container title is still `Source Control`; the content headers are
`REPOSITORIES`, `CHANGES`, and `GRAPH` (localized through the Sakura resources).

The current layout registry deliberately registers only `workbench.scm` as the
selectable Changes View.  Upstream's Repositories View is hidden by default and
Graph is conditional on `scm.historyProviderCount != 0`; this registry currently
has neither `hideByDefault` nor a provider-driven `when` condition.  Registering
the two IDs now would incorrectly make a history-less Graph active and
selectable.  Their exact IDs are reserved in `WorkbenchIds.h`, and the native
stack is the presentation boundary until that capability model exists.

**Superseded record (do not restore):** Graph was
`ScmGraphPresentation{ Unsupported }` -- no history rows, no `git log`, no hit
target, and a resource-owned "not available yet" message.  That state no longer
exists.  See "The Graph View" below for what replaced it.

## The Graph View (2026-08-20)

`workbench.scm.history` now renders a real history.  `GitHistoryModel.h`/`.cpp`
is the pure half -- it has no HWND, runs no git, and reads no file -- and
`CScmWorkbenchTool` is the only thing that turns its rows into pixels.

- One `git log --topo-order --max-count=50` with a `%x1f`-separated,
  `%x1e`-terminated format.  Topological order is what puts a parent below its
  child, which is the only order a swimlane walk can be built from, and the two
  ASCII separators are chosen because neither can appear inside a field a commit
  supplies.  The query runs in the same background worker as the status refresh
  and only after the status reported a repository, so an idle tick still costs
  one refresh rather than two.
- `BuildScmHistoryGraph` is upstream's `toISCMHistoryItemViewModelArray`
  swimlane walk: a lane waiting for this commit becomes its circle and continues
  into its **first** parent, further lanes waiting for the same commit are
  merges that end here, and every additional parent opens a new lane.  A commit
  nothing was waiting for starts its own lane at the right-hand end.  A first
  parent is retained even when another lane already awaits that same commit:
  the duplicate is the branch lane needed on the following row to draw the
  connector back into the shared parent, not a duplicate visible commit.
- `EScmGraphPresentationStatus` is now `Unavailable` / `Available`.
  `Unavailable` still means "no history has been read, or reading it failed",
  and still paints the message rather than an empty list, because an empty list
  would be a claim that the repository has no commits.
- The rows live in a second owner-drawn LISTBOX with its own shared
  `controls::COverlayScrollbar`, rather than a hand-scrolled canvas, so the
  Graph inherits the keyboard, wheel, and scrollbar behaviour the change list
  and the Explorer already have.

## The Graph row's geometry, badges, and menu (2026-08-21, #238)

Read from `vs/workbench/contrib/scm/browser/scmHistory.ts` and
`scmHistoryViewPane.ts`, not from a screenshot.

- **Lane `n` is at `SWIMLANE_WIDTH * (n + 1)`.** `drawCircle` offsets by a whole
  swimlane, not half of one, so the first lane sits 11 DIP in from the graph's
  left edge. `laneX` reproduces that.
- **Three node shapes, not one.** `renderSCMHistoryItemGraph` draws HEAD as a
  `CIRCLE_RADIUS + 3` disc with a `CIRCLE_STROKE_WIDTH` hole, a multi-parent
  commit as `CIRCLE_RADIUS + 2`, and every other commit as `CIRCLE_RADIUS + 1`.
  `PaintGraphRow` draws exactly those, and the hole takes the row's own
  background so the current commit reads as a ring the way upstream's does.
  `kGraphCircleStrokeWidthDip` is upstream's `CIRCLE_STROKE_WIDTH`.
- **The badges trail the text.** `HistoryItemRenderer.renderTemplate` appends
  the graph, then one `IconLabel` whose label is the subject and whose
  description is the author, then the badge container. The row therefore reads
  subject, author, badges -- the previous order, which put the badges between
  the graph and the subject, was a divergence. The badges are measured before
  the text is drawn so the subject ellipsizes into what they leave, which is
  what the flex row does.
- **Only the first ref shows its name.** Upstream renders the first *coloured*
  reference with its description and every later group as icon plus count. Ours
  names the first ref and gives the rest their kind's Codicon alone
  (`GraphRefIcon`: `$(git-branch)`, `$(cloud)`, `$(tag)`).

Recorded divergences for that row:

- **A badge takes the commit's own lane colour, not the ref's.** Upstream colours
  each badge from `ISCMHistoryItemRef.color` -- `chartsBlue` for a branch,
  `chartsPurple` for a remote, `#EA5C00` for the base ref -- and hides uncoloured
  refs (tags) under the default `scm.graph.badges: filter`. Those colours are not
  published by `theme::ThemePalette` either, so every badge here uses the lane
  colour the commit's circle already uses, and a tag is rendered rather than
  filtered. Revisit with the lane-colour token work below.
- **The author date is no longer drawn.** Upstream's row carries the subject and
  the author; the date lives in the hover. Right-aligning `author, date` was a
  divergence, and it is removed rather than kept.
- **A badge group carries no count.** Upstream collapses same-colour, same-icon
  refs into one badge with a count. Ours renders one badge per ref.

## The Graph row's context menu (2026-08-21, #238)

Right-clicking a Graph row opens upstream's `scm/historyItem/context` menu,
built by `BuildGitHistoryItemContextMenu` and tracked by
`ShowHistoryItemContextMenu`. It follows the same rules the resource menus
follow: the row is selected first, the commit id is copied before
`TrackPopupMenu` pumps messages, and a row the Graph no longer holds fails
closed (`CScmWorkbenchTool::HistoryItem` answers nothing).

- Only the `9_copy` group ships: `git.copyCommitId` (`Copy Commit Hash`) and
  `git.copyCommitMessage` (`Copy Commit Message`), both registered with real
  executors in `RegisterGitCommands`.
- **Every `git.graph.*` entry is absent, not disabled**:
  `git.graph.checkoutDetached`, `git.graph.cherryPick`,
  `git.graph.compareWithRemote`, `git.graph.compareWithMergeBase`,
  `git.graph.compareRef`, and the whole `scm/historyItemRef/context` menu
  (`git.graph.checkout`, `git.graph.deleteBranch`, `git.graph.deleteTag`). None
  has a route here, and `git.branch` / `git.createTag` would act on HEAD rather
  than on the clicked commit, so offering them would name the wrong operand.
- `git.copyCommitMessage` copies the commit's **full** message. `GitHistoryItem`
  gained `message` (`%B`) for exactly that: copying the subject would be a
  different text wearing the same label.

## The Repositories row's toolbar (2026-08-21, #238)

`RepositoryRenderer` sets the row's toolbar to
`[...statusBarCommands, ...scm/title navigation]` with the remaining `scm/title`
groups in the overflow `...`. `LayoutBand` now appends
`BuildGitScmTitleToolbarActions` (`git.commit` / `$(check)`, `git.refresh` /
`$(refresh)`) after the published `statusBarCommands`, then an
`EBandSegment::Overflow` segment that opens `BuildGitScmTitleOverflowMenu`
(`git.pull`, `git.push`, `git.clone`, `git.checkout`, `git.fetch`, a separator,
`git.showOutput`).

- `git.refresh` and `git.showOutput` are new registry entries with real
  executors: `CScmWorkbenchTool::Refresh()` and revealing the Output panel
  through `CBottomPanelTool::ShowOutput()`.
- **Upstream's eight `2_main` submenus are absent** (`git.commit`,
  `git.changes`, `git.pullpush`, `git.branch`, `git.remotes`, `git.stash`,
  `git.tags`, `git.worktrees`). They are submenus of commands this fork mostly
  does not have, and the ones it does have are already reachable from the flat
  overflow above.
- **The new toolbar tooltips and menu labels ship in English.** The
  `STR_WORKBENCH_COMMAND_GIT_*` ids exist but no `.rc` string table defines
  them, so every Git command title here already falls back to its English
  literal. Localizing them is one change across all three resources, not a
  per-entry decision.

## The Graph header's toolbar (2026-08-21, #238)

The Graph pane header carries `MenuId.SCMHistoryTitle`'s `navigation` group,
which `SCMHistoryViewPane` registers with `titleMenuId: MenuId.SCMHistoryTitle`.
`BuildGitScmHistoryTitleToolbarActions` builds it and `LayoutGraphHeaderActions`
right-aligns it in the header, shrinking the title so a long one ellipsizes
rather than drawing under the buttons.

The toolbar shares `bandSegments` with the repository row.  That is deliberate:
one hover model, one tooltip model, and one dispatch path serve both, so a
header button cannot drift out of sync with a row button.  `LayoutBand` now runs
even when no repository row is rendered, and `ToggleSectionAt` tries the
segments before the collapse twistie -- otherwise pressing Refresh would
collapse the pane.

What ships: `git.fetchAll` (`$(git-fetch)`), Pull (`$(repo-pull)`), Push
(`$(repo-push)`), Refresh (`$(refresh)`).

Recorded divergences:

- **Pull and Push route to `git.pull` / `git.push`, not to upstream's
  `git.pullRef` / `git.pushRef`.**  The ref-scoped pair operates on the history
  item reference the Graph's filter names.  With no reference filter here, that
  reference is always HEAD, so the two commands are the same operation; the
  icons stay upstream's.
- **Refresh routes to `git.refresh`, not `workbench.scm.action.graph.refresh`.**
  There is no view-scoped refresh here; `git.refresh` refreshes this Graph along
  with the rest of the view, which is a superset of what the view action does.
- **The repository picker and the history-item reference picker are absent.**
  The first is upstream-gated on more than one provider, and there is only ever
  one here.  The second (the `自動` / `Auto` button) opens a quick pick that
  rewrites the Graph's reference filter -- state this Graph does not keep, and
  a button that could not change anything would be a fake capability.
- **`Go to Current History Item` (`$(target)`) is absent.**  Upstream gates it
  on `SCMCurrentHistoryItemRefInFilter`, which is the same missing filter state.
- **The `...` overflow is absent.**  Its only entries are `View as List` and
  `View as Tree`; this Graph has one presentation, so a toggle there would
  change nothing.
- **`git.pushRef` never becomes `git.publish`.**  Upstream swaps the two on
  `scmCurrentHistoryItemRefHasRemote`.  The repository row already publishes
  `git.publish` through `statusBarCommands` when it applies, so the Graph header
  shows Push unconditionally rather than duplicating that switch here.

## The Changes row's inline actions (2026-08-21, #238)

`scm/resourceGroup/context`'s `inline` group is an always-visible action bar on
the group header row, not a context-menu-only contribution.  Its absence was why
the `Changes` row had a count and nothing else.
`BuildGitResourceGroupInlineActions` builds it and `GroupRowActions` lays it out
right to left from the row's count.

- `Changes` (`workingTree`, `git.untrackedChanges` `mixed`): `git.cleanAll`
  (`$(discard)`) then `git.stageAll` (`$(add)`), which is the contributed order
  within `inline@2`.
- `Staged Changes` (`index`): `git.unstageAll` (`$(remove)`).
- `Merge Changes` and `Untracked Changes`: nothing.

`GroupRowActions` is called by both the row's paint and its hit test, so a
button can never be drawn where a press does not land.  A press on an action is
consumed before the row's collapse gesture.

Recorded divergences:

- **`inline@1` is absent for every group**: `git.viewChanges`,
  `git.viewStagedChanges`, and `git.viewUntrackedChanges` open a multi-file diff
  editor, which has no route here.
- **The merge group has no inline action.**  Its only contribution is
  `git.stageAllMerge`, which is not registered; the row is left bare rather than
  given `git.stageAll`, which would stage a different set.
- **`config.git.untrackedChanges != mixed` yields no actions.**  The
  `*Tracked` / `*Untracked` variants upstream substitutes there are not
  registered.  This product publishes `mixed`, so the branch is unreachable in
  practice and fails closed if that ever changes.

## The Graph row's path geometry (2026-08-21, #238)

`PaintGraphRow` reproduces upstream's `renderSCMHistoryItemGraph`
(`vs/workbench/contrib/scm/browser/scmHistory.ts`) path for path, not just its
lane colours and node shapes.  Drawing only vertical lane segments was the
earlier state, and it made a merge unreadable: nothing on screen showed where
two lanes joined.

- **`gx(k)` is upstream's `SWIMLANE_WIDTH * k`.**  Lane `n` is centred on
  `gx(n + 1)`, the row's vertical middle is upstream's `SWIMLANE_HEIGHT / 2`,
  and `SWIMLANE_CURVE_RADIUS` is 5 DIP.  Every constant is scaled through
  `icons::ScaleDip`, so the geometry is identical at any DPI.
- **The five paths are upstream's own**, walked with a separate
  `outputSwimlaneIndex` cursor exactly as upstream walks it:
  1. a second lane reaching this same commit draws `/` then `-` into the circle
     (`index != circleIndex`) -- a merge's incoming side;
  2. a lane that kept its position draws one full-height `|`;
  3. a lane that shifted left draws `|`, a curve, the horizontal run, a second
     curve, `|`;
  4. **every parent after the first draws `-` out of the circle and `\` down
     into that parent's own lane, in that parent's colour** -- this is the
     stroke that makes a merge legible, and the one this row previously lacked
     entirely;
  5. `|` into the circle in the arriving lane's colour and `|` out of it in the
     circle's colour, so the two halves of the row can differ.
- **`circleIndex` and `circleColor` follow upstream's rules**, not the model's
  convenience: the commit sits on the input lane that was waiting for it, or on
  a new lane just past the right-hand end, and the colour is read from the
  output lane at that index first, the input lane second, the row's own
  assignment last.
- **SVG quarter arcs are drawn as cubic Beziers.**  `A r r 0 0 s` between two
  points whose tangents are axis-aligned is approximated by control points
  0.5523 of the way from each end toward the corner those tangents meet at.
  GDI's `PolyBezierTo` continues from the current position, which `Arc` cannot
  do; the divergence is the approximation itself, and it is under half a pixel
  at these radii.
- **A shared first parent keeps both incoming lanes until the parent row.**
  `BuildScmHistoryGraph` intentionally preserves the second lane even though
  both lanes carry the same parent id.  `PaintGraphRow` consumes that lane as
  the branch connector; collapsing it in the model makes the new-branch line
  stop at the child commit instead of rejoining the shared parent.

### Graph divergences


- **The lane colours are literals, not theme tokens.**  Upstream registers
  `scmGraph.foreground1` .. `foreground5` and reads them from the colour theme.
  `theme::ThemePalette` publishes no such tokens, so `kGraphLaneColors` carries
  the registered upstream **defaults**: `#FFB000`, `#DC267F`, `#994F00`,
  `#40B0A6`, `#B66DFF`.  The last two were wrong until 2026-08-21 -- they had
  been copied from a different palette -- so re-read them from upstream rather
  than trusting the values here.  Replace the whole array with palette reads
  when the tokens are published; do not add a sixth colour of our own.
- **Superseded: "a row has no hover actions, no context menu, and no click
  command."**  A row now has upstream's `scm/historyItem/context` menu; see
  "The Graph row's context menu" above for what it contains and what it omits.
  A row still has no hover action bar and no click command, because opening a
  commit's changes needs a commit-detail input that does not exist here.
- **The page is a fixed 50 commits with no incremental loading.**  Upstream
  pages as the user scrolls.  A bounded page keeps the refresh cost fixed; the
  bound is not hidden, because the list simply ends.
- **Only `%D` decorations are badged.**  Upstream also renders history-item
  labels a provider contributes.  Our provider contributes none.

## Pane headers draw the side bar's section separator (2026-08-21, #238)

Every pane header except the topmost draws the same 1px `border` line the
Explorer's Outline header draws, because in VS Code each pane in a view
container is separated from the one above it. The topmost header is the
exception: the container's own title already sits above it, and a line there
would double that boundary. `PaintViewHeader` decides that by comparing the
header's top against the client top, which is the same origin `ViewStack()`
lays the first header out at.

## Collapsing and Resizing the Sections

The three sections behave like upstream's panes: each header carries a
`chevron-down` / `chevron-right` twistie that collapses its body, and the
Changes/Graph boundary carries a `.monaco-sash` drag handle.

- A collapsed section keeps its header and gives up its whole body, because a
  section with no header could not be reopened.  The commit box and the Commit
  button collapse **with** the Changes section, since upstream's `.scm-editor`
  lives inside that pane rather than above it.
- The sash is a 4-DIP overlay straddling the boundary, exactly as upstream's is:
  it consumes no layout space, so collapsing or hiding the Graph simply removes
  it rather than leaving a gap.
- The dragged size is stored in DIP (`graphBodyDip`), not pixels, so a DPI
  change rescales it, and the delta is measured against the drag's own origin so
  repeated rounding cannot accumulate.  `BuildScmViewStackLayout` clamps it
  against `minimumBodyHeight`, so the Graph can never starve the change list.
- Recorded divergence: the sizes are **not persisted**.  Upstream stores pane
  size and collapsed state in its view-state memento.  There is no memento key
  for this view yet, so both reset to their defaults with the window.

## The Repository Row

The band below the Repositories View header and above the Changes View header is
upstream's
`RepositoryRenderer` (`vs/workbench/contrib/scm/browser/scmRepositoryRenderer.ts`),
read verbatim rather than inferred from a screenshot.

- Template order, left to right: the icon, the `IconLabel`, the actions toolbar,
  the `CountBadge`. `CScmWorkbenchTool` lays the band out in that order and
  right-aligns the count, then the actions. The count occupies no width under
  the default `scm.providerCountBadge` policy below, exactly as upstream's
  `display: none` removes it from the flex row.
- The icon is `ThemeIcon.isThemeIcon(provider.iconPath) ? provider.iconPath :
  Codicon.repo`, and `repoSelected` applies only when more than one repository is
  visible. A single repository therefore always renders a plain `$(repo)`.
- The label is `provider.name` rendered with `IconLabel(..., { supportIcons:
  false })`. The repository name is **never** parsed for `$(name)` tokens; a
  directory literally called `$(x)` must render as itself. `description` is
  `undefined` for a single repository, so the band shows none.
- The row's title is `${provider.label}: ${labelService.getUriLabel(rootUri)}`,
  i.e. `Git: C:\path\to\repo`, falling back to `label` alone with no `rootUri`.
  It reaches the user through the band's tooltip.
- The toolbar is `statusBarCommands.map(c => new StatusBarAction(c,
  commandService))` followed by the menu actions. The band therefore renders the
  **same published `ScmCommand` values** the status bar renders and runs them
  through the same `CommandCallback`, so the branch item and the row can never
  mean different things. The menu actions have no native counterpart yet and are
  absent rather than approximated.

### Native band divergences

- **`scm.alwaysShowRepositories` is hard-coded to `true`.** Upstream renders the
  repository row only when `visibleRepositories.length > 1 ||
  scm.alwaysShowRepositories === true`. With one repository and the default
  setting, upstream shows no repository name, no path, and no branch inside the
  view at all. That is precisely the operational-safety gap this work
  exists to close — "which repository, which branch" must be visible before a
  commit — so the row is always rendered here. This is a deliberate divergence
  from a **default**, not from a capability: the concept, its identifiers, its
  contents, and its ordering are upstream's, and the setting's other value is
  what upstream users already see. Revisit when `scm.*` settings are readable
  from configuration.
- **The band is painted by the tool, not by the resource list.** The provider
  band is drawn in `CScmWorkbenchTool`'s own `WM_PAINT` and hit-tested in its
  window proc, between the 30-DIP Repositories header and the Changes header.
  Its segments are recomputed on every re-layout, and `WM_LBUTTONUP` invokes
  the segment under the cursor. This mirrors `CMainStatusBar`'s existing
  `m_statusbarHitTargets` design rather than inventing a second interaction
  model.
- **`scm.providerCountBadge` is hard-coded to its documented default, `hidden`,
  so the row shows no count.** Upstream sets
  `countContainer.setAttribute('data-count', String(count))` and toggles
  `hide-provider-counts` / `auto-provider-counts` on the tree container from that
  setting; `scm.css` then hides `.scm-provider > .count` outright under `hidden`
  and only at `data-count="0"` under `auto`. **Corrected record:** an earlier
  version of this entry inferred a zero-only rule from the `data-count` attribute
  without reading the stylesheet, and the band rendered a nonzero count. That was
  a divergence from a stock VS Code, which shows nothing here. The count is still
  computed exactly as upstream computes it
  (`provider.count ?? getRepositoryResourceCount(provider)`), so `auto` and
  `visible` need only the setting to be readable.
- **`Source Control` is the ViewContainer title, not an in-content SCM header.**
  Current VS Code's content View titles are `Repositories`, `Changes`, and
  `Graph`. The native host renders those same sibling headers with no trailing
  count; repository/resource counts remain in their own rows. The former
  `SOURCE CONTROL  (N)` text is not a valid substitute for either a ViewContainer
  title or a Repositories header.
- **The band's tooltips are Win32 tooltips, not hovers.** The name segment shows
  the row title and each action segment shows its command tooltip, delivered
  through `LPSTR_TEXTCALLBACKW` / `TTN_GETDISPINFOW` because the strings change
  with every refresh. Upstream renders a `HoverWidget`; the richer surface this
  repository already has (`workbench/hover/`) is not wired here yet, so a plain
  tooltip is a degraded presentation of the same text rather than a different
  one.
- **`git.branchProtection`'s `$(lock)` icon case is absent.** Upstream's `getIcon`
  has a protected-branch branch that depends on that setting. The setting is not
  read, so the case does not exist here rather than being approximated by an
  unrelated condition.
- **`syncTooltip`'s read-only-remote branch is not evaluated.** Upstream's first
  case also covers a remote it knows to be read-only. **Updated record:** the
  remote commands now do read that fact — `ParseGitRemotes` derives
  `GitRemote::isReadOnly` exactly as upstream does, and `RunGitSync` stops at
  `the remote is read-only, so nothing was pushed`. The *tooltip* still does not,
  because `GitScmPublisher` runs no `git remote --verbose`: the publisher refreshes
  on a 5-second timer and adding a second git invocation to it would pay for that
  string on every idle tick. So only the "nothing to push" half of the condition
  applies to the wording, and a read-only remote with local commits still gets the
  push tooltip — but invoking it now reports the real reason instead of failing at
  git. Revisit when the publisher's refresh carries remote metadata.
- **`Publish to {0}` is simplified to `Publish Branch`.** Upstream names the
  remote when exactly one remote-source publisher is registered and says
  `Publish to...` otherwise. There is no publisher registry here, so the general
  form is used rather than naming a remote that was never resolved.

## Branch Commands

The status bar's branch item runs `git.checkout`, exactly as in VS Code. All
four commands are registered under upstream's own IDs — `git.checkout`,
`git.checkoutDetached`, `git.branch`, `git.branchFrom` — through
`RegisterGitCommands`, which is a separate atomic batch from the workbench
shell's `RegisterBuiltinCommands` because upstream ships them from `vscode.git`
rather than from the workbench itself.

`GitBranchCommands.h/.cpp` owns the orchestration and takes its Quick Pick,
input box, git invoker, and message sink as injected callables. It has no HWND,
no `CEditWnd`, and no `SourceControlService` dependency, so every flow below is
asserted without a window (`GitBranchCommands.*`).

- Titles, placeholders, and prompts are upstream's published strings:
  `Checkout to...`, `Checkout to (Detached)...`, `Create Branch...`,
  `Create Branch From...`, `Select a branch or tag to checkout`,
  `Select a branch to checkout in detached mode`,
  `Select a ref to create the branch from`, `Branch name`,
  `Please provide a new branch name`.
- Checking out a **remote head** reproduces `CheckoutRemoteHeadItem.run`:
  `for-each-ref --format %(refname:short)%00%(upstream:short) refs/heads` finds a
  local branch whose upstream is exactly that ref and checks that branch out;
  with no such branch, `checkout -q --track <remote/ref>` creates it. Checking
  the remote ref out directly would detach HEAD, which is a different operation
  the user did not ask for. The lookup is restricted to `refs/heads` so a remote
  ref cannot match itself.
- The listing and the tracking lookup share one decoder, `DecodeGitOutput`. Two
  decoders that disagreed by one character would silently turn "switch to the
  existing local branch" into "create it again".
- `promptForBranchName`'s order is preserved: existing-branch collision, then the
  sanitize notice. A collision re-asks with the rejected text still in the field;
  a sanitized name is accepted without a second ask. An empty box is upstream's
  cancel path, not an error.
- A failed listing never opens a picker. An unreadable ref list and an empty one
  are different facts, and an empty picker would render the second as the first.
- A non-zero exit reports git's own trimmed stderr ("local changes would be
  overwritten", "pathspec did not match"); the hand-written sentences cover only
  the terminal states where git produced no output at all.

## Resource and Group Context Menus

`GitScmMenus.h/.cpp` is a pure model of upstream's `scm/resourceState/context`
and `scm/resourceGroup/context` contributions, read from the built-in Git
extension's own `package.json` and `package.nls.json` rather than from memory.
It takes no HWND and knows nothing about `TrackPopupMenu`; `CScmWorkbenchTool`
is the only thing that renders it.

- Ordering is upstream's `MenuInfo._compareMenuItems`: the `navigation` group
  first, the remaining groups alphabetically (`1_modification` < `2_view` <
  `worktree_diff`), and within one group by `order` and then by **title**.
  `Discard Changes` therefore precedes `Stage Changes`, and `Discard All
  Changes` precedes `Stage All Changes`, because each pair shares a group and an
  order and upstream breaks that tie by title.
- Titles are upstream's **bare** `package.nls.json` strings.
  `WorkbenchCommandDescriptor::title` carries the category-prefixed `Git: Stage
  Changes`, which belongs to the Command Palette; a menu must not reuse it.
- A row names its operand through a side table, `GitPublication::operands`,
  keyed by `(resourceUri, group)`. Both halves are required: the same path
  legitimately occupies a row in Staged Changes and a row in Changes, and those
  two rows stage and unstage different things. The table is emitted by
  `AppendStageResources`, the same single derivation `CollectGitStageResources`
  uses, so a row the view renders and the operand a menu names cannot drift. Its
  order follows the change walk, not group order; consumers join on the key.
- Group-scoped entries pass **no** arguments. Upstream's `stageAll`, `unstageAll`
  and `cleanAll` read the repository's own groups, and so do ours through
  `CollectGitStageResources`.
- `git.stage` is kept on a Merge Changes row because upstream contributes it
  there. Ours returns a typed `UnsupportedMergeConflict` with an actionable
  message, which is a reported boundary rather than a silent no-op.

### Native menu divergences

- **Every upstream entry whose command has no route here is absent**, not
  rendered disabled and not approximated by a different command. The omitted set
  is `git.openHEADFile`, `git.openFile2`, `git.ignore`,
  `git.revealFileInOS.{linux,mac,windows}`, `git.revealInExplorer`,
  `git.compareWithWorkspace`, `git.stageAllMerge`, `git.viewChanges`,
  `git.viewStagedChanges`, `git.viewUntrackedChanges`, `git.stageAllTracked`,
  `git.stageAllUntracked`, `git.cleanAllTracked`, and `git.cleanAllUntracked`.
  Merge Changes and Untracked Changes headers therefore have **no** group menu at
  all, and so does a Changes header under any policy other than `mixed`. A caller
  that receives an empty model must show no popup: an empty menu would claim the
  row has actions that merely happen to be unavailable.
- **`git.untrackedChanges` is hard-coded to its documented default, `mixed`.**
  That is the policy both publish paths already use, so the menu's untracked
  branch matches the groups its rows were built from. Revisit with the rest of
  the `git.*` settings.
- **Upstream's `inline` actions are not rendered.** The contributions place
  `git.stage` / `git.clean` / `git.unstage` at `inline@2`, which VS Code draws as
  hover buttons on the row. The native owner-drawn list now supplies the tree
  affordances (chevrons, resource icon, right-aligned status decoration, and
  hover/selection states), but it does not claim an inline toolbar it cannot
  fully model. Those actions remain reachable through the context menu. The
  same commands, with the same operand, run either way. **Corrected record:**
  an earlier version of this entry called the `inline@1` slot "an open-diff
  action" and elsewhere called it `git.openChange`. Upstream contributes *both*,
  under complementary `when` clauses: `git.openFile2` when
  `config.git.showInlineOpenFileAction && config.git.openDiffOnClick`, and
  `git.openChange` when the same setting is `false`. `git.openDiffOnClick`
  defaults to `true`, so the action a stock VS Code actually draws there is
  `git.openFile2`, which is why that id stays in the omitted set above while
  `git.openChange` has left it.
- **`git.openChange` is in `navigation` for Index, Changes, and Untracked
  Changes, and deliberately not for Merge Changes.** Read from the built-in Git
  extension's own `package.json` rather than from memory: the `merge` group's
  `navigation` contains `git.openFile` alone. A conflicted file is opened, not
  compared, so adding an Open Changes entry there would offer a comparison
  upstream does not. Within `navigation` every entry carries no explicit
  `order`, so `MenuInfo._compareMenuItems` breaks the tie by title and
  `Open Changes` precedes `Open File`.
- **The menu is selected by the row, and a row we did not publish gets none.**
  `ParseGitResourceGroupId` returns nothing for a group id no built-in Git group
  publishes, and a non-Git provider's rows are therefore never given Git's menu
  — it would offer to stage something Git does not own. A
  resource row with no operand is refused for the same reason: a menu that cannot
  say what it would act on is worse than no menu.
- **Right-click selects the row first**, as VS Code's list does, so the row the
  user is looking at and the row the command receives are the same row. The
  menu-key path has no cursor position and anchors on the focused row's bottom
  left instead.
- **The row is copied before the menu is tracked.** `TrackPopupMenu` pumps
  messages, so a background refresh can rebuild the row vector while the menu is
  open; holding a reference across it would name a row that no longer exists.
- **Multi-selection is not offered.** Upstream's handlers take the whole
  selection; the list is single-select here, so every menu invocation names
  exactly one row. `BuildGitStageArguments` already carries a vector, so a
  multi-select list needs no model change.

## Commit Commands and the Input Box

`GitCommitCommands.h/.cpp` is a pure model of the built-in Git extension's
`Repository.commit`, `commitWithAnyInput`, `handleCommitError`, and
`undoCommit`, read from `microsoft/vscode`'s own `extensions/git/src/commands.ts`
and `repository.ts`. It takes its git invoker, confirmation presenter, message
prompt, dirty-document enumerator, document saver, and message sink as injected
callables, so every flow is asserted without a window (`GitCommitCommands.*`).
`git.commit`, `git.commitAmend`, and `git.undoCommit` are registered under
upstream's own IDs through the same `RegisterGitCommands` batch as the branch
and working-tree commands.

- The message always travels through `--file -` on stdin, never as an argument.
  `RunGit` writes stdin on its own thread, so a message larger than the pipe
  buffer cannot deadlock; an argument would additionally be length-bounded and
  could be reread as an option.
- Upstream emits `--allow-empty-message` **twice** for a non-empty message — once
  for the message and again on its `!useEditor` branch — and `-c
  user.useConfigOnly=true` is spliced at the front, not appended. Emitting the
  flag once would be tidier and would also be a different command line.
- Gate order is upstream's and is load-bearing: unsaved documents, then the
  smart-commit suggestion, then the no-changes/empty-commit offer, then the
  `--no-verify` gates, then message resolution. Each one can end the command, so
  reordering them changes which prompt a user sees.
- `add([], …)` is `add -A -- .`: the **whole worktree**, not a listing of the rows
  the last refresh happened to know about. A file changed since that refresh is
  therefore included, exactly as upstream.
- Saving unsaved documents re-adds only the documents that belong to the index
  group, because saving them made the worktree differ from what was staged.
- An amend over an existing commit deliberately yields **no** message, so
  `--amend --no-edit` keeps the previous one. An empty message with nothing to
  amend commits nothing.
- A failed commit returns no input-box value at all, which is what keeps the
  message that failed from being discarded. Only success clears the box.
- `undoCommit` is `reset --soft HEAD~`, which keeps the undone commit's content
  staged; a mixed or hard reset would unstage or destroy it. The first commit has
  no parent, so it is `update-ref -d HEAD` followed by `rm --cached -r -- .`. The
  undone message is read **before** the reset and restored into the box.
- `handleCommitError`'s order is preserved: git's own stderr is matched first, and
  only then is git asked whether `user.name` / `user.email` are configured, so a
  failure that merely mentions a name cannot be reported as a missing identity.

### Commit divergences

- **Merge and rebase state are read from `git rev-parse --absolute-git-dir`,
  not from a literal `<root>/.git` join.** Upstream joins the path. A worktree,
  a submodule, and a `.git` file all put the metadata directory elsewhere, and
  the joined path would then miss `MERGE_HEAD` and report "no merge in progress"
  for a repository that is mid-merge. One extra invocation buys a correct answer.
- **A rebase in progress fails closed as
  `EGitCommitCommandStatus::UnsupportedRebaseInProgress`.** Upstream's
  `Repository.commit` never reaches `git commit` there; it runs `git rebase
  --continue`. There is no rebase model here, so the command refuses with an
  actionable message rather than writing a commit the user did not ask for.
  The state is read once, before any prompt, so the user is not walked through a
  confirmation for something that cannot happen.
- **`git.useEditorAsCommitInput` is hard-coded to `false`, against an upstream
  default of `true`.** This is the one place a hard-coded default deliberately
  differs from upstream's. That setting makes git open `core.editor` on a commit
  message file and makes VS Code itself the editor; there is no such editor
  integration here, so honouring the default would hand the message to whatever
  `core.editor` happens to be — frequently a console editor with no window — and
  the commit would appear to hang. `false` is the value that keeps the message in
  the SCM input box, which is the surface this work exists to make usable.
- **Upstream's `Always` / `Never` and `OK, Don't Ask Again` buttons are
  absent.** Each writes a `git.*` setting, and there is no Settings writer on
  this path. A button that silently failed to persist the user's choice would be
  worse than no button; the remaining choices are upstream's own.
- **The unsaved-documents prompt can only see this editor process's own
  document.** Upstream enumerates every dirty `workspace.textDocument` inside the
  repository. An editor process owns exactly one document, so the prompt names at
  most that one, and a dirty document in another window is neither named nor
  saved. Revisit when a cross-window document authority exists.
- **The post-commit input reset goes to empty, not to `commit.template`.**
  Upstream's `commitOperationCleanup` resets the box to `getInputTemplate()`,
  which reads `git config commit.template` and loads that file. Reading it is not
  implemented, so the reset is to empty.
- **The diagnostics and branch-protection commit hooks are not implemented.**
  Upstream's commit path also consults `git.diagnosticsCommitHook` and
  `git.branchProtectionPrompt`. Neither setting is readable and neither model
  exists here, so those gates do not exist rather than being approximated.
- **The commit confirmations are native task dialogs, so upstream's one
  non-modal `showInformationMessage` becomes modal here.** `BuildNoChangesPrompt`
  carries `modal = false` and `warning = false` so the model still records which
  of upstream's two message functions produced it, and the presenter renders the
  information icon rather than the warning icon; only the modality differs.
  Revisit when a native notification producer exists — see the branch-command
  entry below for why there is none today.
- **The message prompt puts upstream's prompt line into the dialog caption.**
  `SExtensionQuickInputRequest` has no `prompt` field, so `Please provide a
  commit message` becomes the caption and the placeholder — the half that names
  the branch being committed on, `Message (commit on "<branch>")` — stays in the
  field. Both are degraded presentations of the same two strings.
- **The input box's background and border use the `raised` and `border` palette
  tokens.** VS Code styles it from `input.background` / `input.border`, which
  this theme palette does not publish. The nearest published tokens are used
  rather than a hard-coded colour that no theme could change.
- **The placeholder is painted by the tool, not by the control.**
  `EM_SETCUEBANNER` works only on a single-line edit, and the commit box is
  multi-line, so the placeholder is drawn in the tool's own paint path.
- **`scm.inputMinLineCount` and `scm.inputMaxLineCount` are real settings.**
  Both are registered in `config/BuiltinConfigurationDescriptors.cpp` under
  upstream's own ids, with upstream's defaults (1 and 10), upstream's 1..50
  bounds, and Profile/Workspace/Folder scope. The view does not read settings
  itself: `CEditWnd::ApplyScmInputLineCountSetting` resolves them through the
  configuration service and hands them over with
  `CScmWorkbenchTool::SetInputLineCountRange`, so the box opens at the minimum
  and auto-grows to the maximum exactly as upstream's `InputRenderer` sizes it.
  Documented divergence: upstream bounds each key independently, and so does the
  registration here; a `scm.inputMaxLineCount` below the effective minimum is
  resolved to that minimum rather than rejected, because a commit box shorter
  than its own minimum has no rendering.
- **`scm.inputFontSize` is still hard-coded to its documented default.** Hard-coding
  the upstream default keeps the box identical to a stock VS Code; inventing a
  third size would not. It becomes a real setting when the tool gains a font
  scale of its own.

## Paint Stability Invariants

The Source Control view is a stack of sibling native windows, so its paint path
must preserve the pixels already on screen when the model has not changed.

- An idle Git refresh may advance no view state. `RebuildRows` compares the
  complete `ScmRow` sequence and resource count; an equal result must not call
  `LB_RESETCONTENT`, `LB_ADDSTRING`, or a parent-wide redraw.
- List hover invalidation is row-local. Moving within the same row does not
  invalidate the list; when a group row's inline action hit target changes,
  invalidate only that row. A row transition invalidates the old and new rows.
- Rebuilds use `RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN` without an
  erase pass, and parent invalidation uses `FALSE`. Every owner-drawn row paints
  its own background, so a background erase would only expose an empty frame.
- The SCM container uses both `WS_CLIPCHILDREN` and `WS_CLIPSIBLINGS`; the list,
  graph, edit box, and overlay scrollbars are siblings and must not paint into
  one another's rectangles.

## Opening a Change

`GitDiffModel.h/.cpp` is a pure model of the built-in Git extension's
`getLeftResource` / `getRightResource` / `resolveChangeCommand` and of the
`git:` URI that `toGitUri` builds, plus the line-level diff and the row
alignment a side-by-side diff editor needs. It takes no HWND, reads no file, and
runs no git; `CEditWnd` is the only thing that turns its output into a surface.

- The published resource command is upstream's own: `vscode.diff` with
  `[left, right, title]` when both sides resolve, `vscode.open` with
  `[resource, options, label]` when only the right-hand side does, and **no
  command at all** when neither does. Both branches are titled
  `localize('open', "Open")`, so a diff and a plain open are named the same
  thing, exactly as upstream names them.
- `vscode.diff` and `vscode.open` are registered as VS Code **API commands**.
  Upstream registers them in `CommandsRegistry` only — no `MenuRegistry`
  contribution, no category, no keybinding — so they carry no surface bindings
  here either, and their `when` clause is `workbenchReady` rather than
  `gitOpenRepositoryCount != 0`: they are workbench commands an extension may
  call, not Git commands. `git.openChange` is the Git command, registered in the
  same `RegisterGitCommands` batch as the stage and commit commands.
- The argument lists cross as JSON through
  [`../commands/ApiCommandArguments.h`](../commands/ApiCommandArguments.h), so a
  `ScmCommand` a native row publishes and a payload an extension sends are the
  same wire shape. `TextDocumentShowOptions.override` is carried rather than
  flattened: the built-in Git provider passes `false` for a both-modified
  conflict and leaves it undefined otherwise, and absent and `false` are
  different requests.
- `sanitizeRef('~')` is one fact **per change**, not per row. A path that is both
  staged and edited again compares its unstaged row against the **index**, not
  against HEAD; comparing against HEAD there would fold the staged edit into the
  diff and show the user changes they have already staged. `stagedInIndex`
  carries that fact from the change walk so the resolver never has to search the
  published groups.
- A `git:` URI round-trips through `BuildGitDiffEndpointUri` /
  `ResolveGitDiffEndpointUri`, and an endpoint whose path will not join onto the
  repository root resolves to nothing. A side that cannot be read cannot be
  published either, so the refusal happens at publication rather than at the
  click.

### Diff divergences

- **The surface is a projection, not an `EditorInput`.** `CDiffSurface` is a
  native composition-layer surface, so `CEditWnd` shows it only while the native editor has no active
  document, and a diff requested while a document is open returns
  `NotApplicable` with that reason. Upstream opens a real diff editor in the
  group. Revisit when a second editor input can be projected.
- **`vscode.open` on a repository-side endpoint is `Unsupported`.** Upstream
  opens a `git:` URI as a read-only document through its `GitFileSystemProvider`.
  There is no read-only document input here, and showing one side of a
  comparison with the other left empty would draw a whole-file insertion git
  never reported. Working-tree endpoints open normally.
- **No character-level inner diff.** Upstream computes one and uses its
  alignment points to align *inside* a changed region. `BuildGitDiffViewRows`
  starts a region level on both sides and pads the shorter side at the region's
  **end**. The rows are therefore a coarser alignment of the same regions, never
  a different set of regions.
- **The diff is bounded by edit distance, not by a clock.** Upstream's
  `IDocumentDiff.hitTimeout` means "this diff is not authoritative" and is set
  when its time budget runs out. A pure model has no clock, so the bound here is
  the quantity that budget was standing in for. The field keeps upstream's name
  and meaning, and `SDiffSurfaceContent::truncated` carries it to the surface so
  a bounded result is never rendered as a complete one.
- **No index-side reverse lookup for a staged rename.** Upstream's
  `getRightResource` additionally searches the index group to pick up a staged
  rename's new name. Porcelain v2 already reports the new name in the change's
  own path, so that lookup would only ever find the same string here.
- **No merge editor.** Upstream routes a both-modified conflict to its
  three-way merge editor when `git.mergeEditor` is on. That setting is not read
  and no merge editor exists, so the conflict follows upstream's other branch and
  opens the working-tree file.
- **`diffEditor.diagonalFill` is drawn as an `HS_FDIAGONAL` hatch brush.** VS
  Code paints CSS repeating stripes at its own angle and spacing. GDI's hatch is
  the nearest primitive; it is the same role, the same theme token, and the same
  meaning — "this side has no line here" — at a coarser rendering.
- **The three diff palette roles are composited at design time.** VS Code's
  registered defaults are translucent (`rgba(155,185,85,.2)`,
  `rgba(255,0,0,.2)`, `#cccccc33` / `#22222233`) and composite over the editor
  background. GDI has no alpha, so `diffInsertedLineBackground`,
  `diffRemovedLineBackground`, and `diffDiagonalFill` are published pre-composited
  over `canvas`. High Contrast registers all three as `null` upstream — it paints
  no wash and no fill at all — and the palette reproduces that absence by giving
  them the window color rather than choosing a highlight color.
- **Both sides are decoded by `DecodeGitOutput`**, which is UTF-8 with a
  Latin-1 fallback. A file in another encoding renders as that fallback rather
  than through the editor's own charset detection. One decoder serves both sides
  deliberately: two decoders that disagreed by a single character would render
  identical text as a change, which is exactly the lie this surface exists to
  prevent.

## Staging Selected Lines

`GitLineStaging.h/.cpp` is a pure model of the built-in Git extension's
`extensions/git/src/staging.ts` — `applyLineChanges`, `toLineRanges`,
`getModifiedRange`, `intersectDiffWithRange`, `invertLineChange`,
`toLineChanges` — and of `Repository.stage`'s object-writing half. It takes no
HWND, reads no file, and runs no git; the executor is the only thing that turns
its output into an invocation (`GitLineStaging.*`).

- **Upstream builds no patch.** `git.stageSelectedRanges` does not construct a
  diff and feed it to `git apply --cached`; it assembles the *complete* new
  index-entry content from the original and modified texts and stages that
  content. `ApplyGitLineChanges` reproduces that assembly line for line. A patch
  path would additionally have to agree with git on context, whitespace, and
  rename detection; the content path has nothing to agree about.
- `GitLineChange` keeps upstream's inclusive-end, 1-based encoding **including
  its degenerate cases**: an insertion is `originalEndLineNumber == 0` and a
  deletion is `modifiedEndLineNumber == 0`. That is a different type from the
  half-open `GitLineRangeMapping` the diff produces, and `ToGitLineChanges` is
  the only conversion. Collapsing the two would silently turn a zero-length
  range into a one-line one.
- Unstaging is the same algorithm with the two sides swapped:
  `InvertGitLineChange` exchanges the original and modified fields, and the
  result is staged as the new index content. There is no second implementation
  to keep in step with the first.
- **A deletion that reaches the last line also drops the previous line's
  terminator**, which is upstream's fix for microsoft/vscode#59670. Without it
  the assembled content keeps a trailing newline the modified text does not
  have, and the staged blob differs from what the user selected by one byte.
  `ApplyGitLineChangesDropsTheTerminatorOfADeletedLastLine` exists to catch
  exactly that.
- The staged content is written with `hash-object --stdin -w --path
  <relativePath>` and installed with `update-index --cacheinfo <mode> <hash>
  <relativePath>`. `--path` is required: without it git applies no clean/smudge
  filter and `core.autocrlf` or a `.gitattributes` rule would be skipped for
  this one write.
- The mode is read from HEAD (`ls-tree -l`), falling back to the index
  (`ls-files --stage`) when there is no HEAD commit, exactly as upstream. A file
  git knows nothing about takes `100644` and `--add`. Guessing `100644` for a
  file that is `100755` in HEAD would silently clear its executable bit.
- The text is normalized to one EOL before any of this, as
  `PieceTreeTextBufferBuilder._getEOL` does: CRLF when more than half the
  terminators carry a CR, otherwise LF. The C++ integer `total / 2` and the
  JavaScript float division agree for both parities, so the boundary case picks
  the same terminator here as in VS Code.

### Line-staging divergences

- **Overlapping selections are unioned, where upstream intersects them.**
  Upstream's `toLineRanges` reduce replaces the accumulated span with
  `l.intersection(last)` when two selections overlap, so selecting lines 1–5 and
  then 3–8 stages **3–5 only** and silently drops 1–2 and 6–8.
  `NormalizeGitSelectedLines` takes the union and stages 1–8. This is a
  deliberate divergence from an upstream defect, not from an upstream design:
  every other case — sorting, merging a span that starts on the line after the
  previous one ends, keeping separated spans apart — is upstream's, and the
  divergence is confined to the one branch where upstream discards lines the
  user selected. Staging less than was selected is the failure mode this work
  exists to prevent.
- **A text with no line terminator at all takes the platform default, CRLF.**
  VS Code's builder falls back to the platform EOL for that input, and this is
  Windows. The value only matters for a single-line file that is about to gain a
  second line, and it is the value a real VS Code on this platform would use.
- **`EncodeGitText` fails closed rather than substituting.** `DecodeGitOutput`
  is UTF-8 with a byte-wise Latin-1 fallback and has no inverse, so staging is
  the first place this subsystem writes bytes. The encoder is told which branch
  the decoder took and refuses — returning no value — when the text cannot be
  represented in it: a character outside Latin-1 under the fallback branch, or
  an unpaired surrogate under UTF-8. A replacement character would stage a blob
  that differs from the file the user was looking at, which is a data-loss bug
  wearing a success return.
- **The empty `--add` slot is omitted rather than passed empty.** Upstream keeps
  an empty string in that argument position when it is not adding. An empty
  argument survives this product's `CommandLineToArgvW` quoting as `""`, which
  git reads as a pathspec matching nothing, so the argument is dropped instead.
  The command line git actually receives is the one upstream means.
- **Every position is validated instead of being allowed to throw.** Upstream
  relies on `TextDocument.lineAt()` throwing for an out-of-range line, which is
  unreachable for the changes its own diff produces. Here every position goes
  through a `validatePosition` equivalent and a reversed range appends nothing,
  so a malformed change list produces a wrong staged content rather than an
  unwind through native window code. On the inputs upstream can reach, the two
  behave identically.

### Selecting the lines to stage

`git.stageSelectedRanges` and `git.unstageSelectedRanges` are registered under
upstream's own IDs in the same `RegisterGitCommands` batch, with upstream's own
`isInDiffEditor` in their `when` clause. `CEditWnd::ExecuteGitSelectedRangesCommand`
is the executor for both; there is one code path because unstaging is the same
algorithm with the two sides exchanged.

- **The operand is a row selection, not a text selection.** Upstream reads the
  active diff editor's `selections` and widens each to whole lines in
  `toLineRanges`. `CDiffSurface` has no caret and no per-glyph measurement, so it
  selects **rows** of the rendered alignment, which is the widened form upstream
  computes anyway. A row that carries a modified line names that line.
- **A row with no modified line names the seam beside it.** That row is a line
  the original side has and the modified side does not.
  `GetModifiedRange` puts such a deletion on the seam between the two modified
  lines that surround it, and `IntersectGitLineChange` reaches that seam from
  either side, so the executor names the seam by the **preceding** modified line
  — or by line 0 for a deletion at the very top, where there is no preceding
  line. Upstream's modified-side selection over those same screen rows would be
  empty, so this is a divergence in the user's favour: selecting a visible
  deletion stages it, where upstream requires the user to also select a
  neighbouring line.
- **The one-line extension cannot swallow an unselected region.**
  `ComputeGitLineDiff` merges adjacent edits into one region, so two distinct
  regions always have at least one unchanged line between them on both sides.
  The neighbour of a pure-deletion seam is therefore always an unchanged line,
  and inside a mixed region the padding rows name that same region's last
  modified line — the region the user was looking at, never the next one.
- **Applicability is gated on what the open comparison is**, exactly as
  upstream's two handlers are, not on which command was invoked. Staging needs a
  working-tree right-hand side; unstaging needs the index (`ref === ''`) on the
  right and HEAD on the left. Any other comparison returns `Unsupported`: there
  is no index entry those lines could be written into, and picking one would
  write somewhere the user did not ask for.
- **Both sides are re-read and compared against what is on screen, and a
  difference fails closed.** Upstream's diff editor holds live documents and
  recomputes its diff as they change; a surface holds a snapshot. A row index
  means nothing against text that has moved, so a changed side reports
  `the compared files changed after this comparison was opened` rather than
  staging a region the user never looked at. A truncated diff (`hitTimeout`) is
  refused for the same reason: a bounded alignment does not name the same
  changes the selection covers.
- **A mixed-encoding comparison fails closed.** The assembled content carries
  text from both sides, so UTF-8 is used only when both sides decoded as UTF-8;
  otherwise the byte-wise fallback applies and `EncodeGitText` refuses anything
  it cannot represent, as described above.
- **Success refreshes the comparison instead of leaving the snapshot stale.**
  The index moved, so `SourceControlService` is refreshed and the same two URIs
  are re-opened through `vscode.diff`, which re-reads both sides —
  `CDiffSurface::ShowDiff` clears the selection, so no selection survives into a
  comparison it no longer describes. A comparison that can no longer be opened
  is retracted rather than left showing text that no longer exists.
- **The selection colour wins over the diff wash.** GDI has no alpha, so a
  selected changed row paints the selection colour rather than compositing it
  over `diffInsertedLineBackground` / `diffRemovedLineBackground` the way VS
  Code's translucent defaults do. Which rows are selected is the fact the user
  is about to act on, so it is the one that must stay legible.

## Remote Commands

`GitSyncCommands.h/.cpp` is a pure model of the built-in Git extension's
`fetch` / `fetchPrune` / `fetchAll` / `pull` / `pullRebase` / `push` / `sync` /
`syncRebase` / `publish` commands, of `Repository.fetch`, `Repository.pull`,
`Repository.pushTo`, `Repository._sync`, and of `getRemotesGit`'s
`git remote --verbose` parse. It takes its git invoker, remote picker,
confirmation presenter, and message sink as injected callables and has **no
HWND** — no `GitSyncCommandContext` member carries one — so every flow is
asserted without a window (`GitSyncCommands.*`, 63 tests).

All nine are registered under upstream's own IDs in the same
`RegisterGitCommands` batch as the branch, stage, and commit commands, because
upstream ships them from `vscode.git` rather than from the workbench.
`CEditWnd::ExecuteGitSyncCommand` is the single executor; `EGitSyncCommand`
names the nine, one member each.

- **Nine commands, not one command with flags.** Upstream publishes the prune,
  all-remotes, and rebase variants as separate command IDs, so they are separate
  members here. Collapsing them into one executor reading a caller-supplied flag
  would invent a payload shape upstream never publishes, and the Command Palette
  would then show one entry where VS Code shows three.
- **Where to push and what to push are read from two different places.** HEAD,
  its upstream, and the ahead/behind counts come from the published
  `GitScmState`; the remotes come from a fresh `git remote --verbose`.
  `BuildGitSyncRepositoryState` joins them without inferring either from the
  other, which is what keeps a stale refresh from choosing the remote.
- **A repository with no remote is a warning, not a failure.** Upstream shows
  `Your repository has no remotes configured to publish to.` and stops;
  `EGitSyncCommandStatus::NotApplicable` carries that sentence to the status bar.
  A detached HEAD declines for the same reason.
- **The sync confirmation is upstream's own, and `git.confirmSync` defaults
  true**, so syncing asks first exactly as a stock VS Code does.
- **`RunGitSync`'s `rebase` parameter is not OR-ed with `git.rebaseWhenSync`
  inside the model.** Upstream's `_sync` computes `rebase || rebaseWhenSync` in
  its caller, so the OR lives at the `CEditWnd` call site: `git.sync` passes
  `configuration.rebaseWhenSync` and `git.syncRebase` passes `true`, which the
  setting cannot turn back off. Putting the OR in the model would make
  `git.syncRebase` indistinguishable from `git.sync` under a true setting.
- A non-zero exit reports git's own trimmed stderr through `DescribeGitFailure`;
  the hand-written sentences cover only the terminal states where git produced no
  output at all. Authentication failures therefore arrive as git's own wording
  rather than as a guess about why the remote refused.
- Success refreshes `SourceControlService`, because a fetch, pull, or push moves
  the counts the branch and sync status items render.

### Remote-command divergences

- **The remote pick strips upstream's leading `$(name)` markup at the presenter
  boundary.** `BuildFetchRemotePickItems` emits `$(cloud) origin` and
  `$(cloud-download) Fetch all remotes`, which is upstream's own label text.
  `CQuickInputDialog` does no codicon parsing at all — verified by
  reading it, not assumed — so those rows would render the literal characters
  `$(cloud) origin`. The executor removes the leading `$(…)` token and the space
  after it, the same treatment the checkout picker's separator rows already get
  there. The model keeps upstream's strings, so a picker that can render
  codicons needs no model change.
- **No `git.*` configuration is read.** `GitSyncConfiguration` is constructed at
  its documented upstream defaults and nothing writes to it: `git.confirmSync`
  (`true`), `git.rebaseWhenSync` (`false`), `git.followTagsWhenSync` (`false`),
  `git.pullTags` (`true`), `git.fetchOnPull` (`false`), `git.autoStash`
  (`false`), `git.allowForcePush` (`false`), `git.useForcePushWithLease`
  (`true`). Hard-coding the upstream default keeps behavior identical to a stock
  VS Code; inventing a third behavior would not.
- **Upstream's setting- and memento-writing buttons are absent.**
  `OK, Don't Show Again` writes `git.confirmSync`, `OK, Don't Ask Again` writes
  the `confirmBranchPublish` memento, and `Always Pull` writes
  `git.autofetch`-adjacent state. There is no Settings writer and no memento
  store on this path, and a button that silently failed to persist the user's
  choice would be worse than no button. The remaining choices are upstream's own.
- **There is no `RemoteSourcePublisher` registry, so publishing never offers to
  add a remote.** Upstream falls back to registered publishers and to its
  `AddRemoteItem` row when a repository has none. Here the no-remote case is
  upstream's warning and nothing else, rather than a row that cannot do anything.
- **`maybeAutoStash` is not implemented, so `git.autoStash` is unreachable.**
  The setting is carried as data at its real default precisely so the gap is
  visible; `GitPullOptions::autoStash` exists and nothing sets it. A pull that
  hit local changes reports git's own refusal instead of silently stashing.
- **There is no git-version probe.** Upstream guards `--autostash` and
  `--force-if-includes` on git >= 2.30. `BuildGitPushArguments` emits
  `--force-if-includes` alongside `--force-with-lease` unconditionally, which is
  currently unreachable — `git.allowForcePush` defaults false and no registered
  command surfaces a force push — so the argument builder is the only place a
  force push can be produced at all. Add the probe before any command reaches it.
- **The fetch pick has no separator row**, for the same reason the checkout
  picker has none: `CQuickInputDialog` cannot render one, and an inert
  selectable line would be a faked capability.
- **No progress indicator and no operation queue.** As with the branch commands,
  each remote command runs `RunGit` synchronously on the UI thread, so the window
  blocks for the duration of one bounded, timeout-guarded invocation. A fetch or
  push over a slow network is where this is most visible; it is the same absent
  queue recorded under "Divergences" below, not a separate gap.
- **Messages go to the status bar, not to a notification**, and the
  confirmations are native task dialogs, so upstream's non-modal
  `showInformationMessage` is modal here. Both are the subsystem-wide boundaries
  already recorded under "Divergences" below.

## Init and Clone Commands, and the Empty-State Welcome Content

`GitInitCloneCommands.h/.cpp` is a pure model of the built-in Git extension's
`init` and `clone` commands (`Repository`-less operations: there is no
repository yet when either one runs) and of `viewsWelcome`'s four Source
Control empty states, read from `microsoft/vscode`'s current
`extensions/git/package.json` and `package.nls.json` rather than from memory.
It takes its folder pick, folder browse, URL prompt, path-existence probe,
confirmation presenter, git invoker, and message sink as injected callables
and has **no HWND**, so every flow is asserted without a window
(`GitInitCloneCommands.*`). `git.init`, `git.clone`, and `git.cloneRecursive`
are registered under upstream's own IDs. The empty-workbench welcome uses
`git.cloneRecursive`; repository initialization uses `git.init`.

- **Current welcome-state contract:** `BuildGitScmWelcomeModel` receives the
  explicit `EGitScmWelcomeWorkspaceState`, not an ambiguous `hasFolder` bool.
  It maps `Folder` to `FolderNoRepository` / `git.init?[true]`,
  `WorkspaceWithFolders` to `WorkspaceNoRepository` / `git.init`,
  `WorkspaceWithoutFolders` to `EmptyWorkspace` /
  `workbench.action.addRootFolder`, and `Empty` to `EmptyWorkbench` /
  `vscode.openFolder`, then `git.cloneRecursive`. An open provider collapses
  every variant to `None`.
- **Single-view merge:** With no provider, `workbench.scm` is the only visible
  SCM view. VS Code's `SCMViewPaneContainer` merges that sole Changes view into
  its `Source Control` container (`mergeViewWithContainerWhenSingleView`), so
  the inner `Changes` header is hidden and allocates no vertical space. The
  native projection has the same explicit layout state; the left-aligned Git
  welcome starts below the container title. Once a provider makes the normal
  SCM stack visible, the Changes header returns.
- **Superseded model (do not restore):** The former two-`viewsWelcome` model was mutually exclusive by upstream's own
  `when` clauses — `view.workbench.scm.folder` fires on
  `workbenchState == folder` and offers `Initialize Repository`;
  `view.workbench.scm.empty` fires on `workbenchState == empty` and offers
  `Open Folder` and `Clone Repository`. This historical two-state description
  is superseded by the explicit `EGitScmWelcomeWorkspaceState` model documented
  above. Do not reintroduce the ambiguous `hasFolder` boolean; repository
  presence still collapses all welcome variants to `None`.
- `git.init`'s command argument is upstream's own: the Command Palette entry
  and the `viewsWelcome` link both invoke it, but the link passes
  `[true]` (`command:git.init?%5Btrue%5D`, URL-decoded), which is
  `skipFolderPrompt`. `RunGitInit`'s fast path — a single open folder, no
  picker shown — exists only because that argument is threaded through, not
  because skipping the picker is this model's own idea.
- `RunGitInit`'s folder resolution follows upstream's `init` command: with
  more than one open folder, or the fast path declined, a Quick Pick lists
  every open folder plus a trailing "Choose Folder..." row
  (`BuildGitInitFolderPickItems`); picking that row opens a folder browser.
  Only a **browsed** folder is subject to the home-directory guard — a folder
  already open was, by definition, already an accepted workspace root.
- The home-directory guard (`IsGitInitHomeDirectoryGuardTriggered`,
  `BuildGitInitHomeDirectoryPrompt`) reproduces upstream's refusal to
  initialize a repository directly in the user's home directory without
  confirmation. `git init` runs from a single resolved working directory that
  is not known until the picker or browser answers, which is why
  `GitCommandInvoker` takes that directory as a parameter — every other
  command family in this directory instead composes its working directory
  once, outside the pure model, because it already operates on a known
  repository.
- `RunGitClonePrepare` / `RunGitCloneExecute` / `RunGitCloneComplete` are three
  separate functions on purpose, matching the shape "prompt and validate,
  then run the long operation, then classify the result" that a non-blocking
  caller needs: the middle phase is the only one that may run off the UI
  thread, and it is the only one that receives a `HANDLE stop`.
- `DeriveGitCloneFolderName` reproduces upstream's own derivation from
  `parseGitmodules`/clone-command URL handling: strip trailing slashes, take
  the last path segment, strip a trailing `.git`. It is used only to name the
  destination subdirectory under the chosen parent directory, exactly as
  upstream's clone flow does before ever invoking git.

### Init/Clone divergences

- **`EGitInitPostAction::OfferToOpen` is data-only.** Upstream's `init`
  command offers to open the newly initialized folder — in this window, a new
  window, or added to the workspace — after a successful `git init` in a
  folder that was not already open. There is no cross-window/open-folder
  capability confirmed to exist on this pass, so `RunGitInit` reports which
  case applies (`AlreadyOpen` vs. `OfferToOpen`) as data and performs no open
  itself. A caller that ignores `OfferToOpen` has not lost correctness, only
  upstream's convenience follow-up.
- **The home-directory guard is a naive string-prefix comparison, not a path-
  descendant check.** `IsGitInitHomeDirectoryGuardTriggered` compares
  `chosenPath.substr(0, homeDirectory.size()) == homeDirectory`. A sibling
  directory that shares every character of the home directory as a prefix
  (for example `C:\Users\devx` against a home directory of `C:\Users\dev`)
  would be misclassified as the home directory itself. Upstream resolves this
  with a real path/URI comparison. Revisit before relying on this guard for a
  destructive-operation gate stronger than "ask once."
- **`RunGitClonePrepare` refuses a `NonEmpty` destination outright, with a
  message, instead of presenting `BuildGitCloneOverwritePrompt` and deleting on
  acceptance.** Upstream deletes the existing directory's contents after the
  user confirms an overwrite. No recursive-delete primitive exists on this
  pass, and showing a confirmation whose "yes" answer this code could not
  honor would itself be a faked capability — the one thing this repository's
  root guidance forbids outright. `BuildGitCloneOverwritePrompt` is still built
  and exported so a future pass that adds a real delete primitive needs no
  model change, only a new branch at the call site.
- **The `git.clone` URL prompt degrades upstream's live-typed Quick Pick to a
  plain input box.** Upstream's clone Quick Pick re-queries as the user types
  (recently opened repositories, GitHub/GitLab suggestions once signed in).
  `CQuickInputDialog` cannot render a live-updating list, so
  `GitCloneUrlPresenter` is a single prompt/placeholder/value input box
  instead. The URL upstream would have resolved either way reaches the same
  `git clone <url>`.
- **The welcome actions are native button rectangles rather than inline
  Markdown links.** The empty workbench keeps upstream's action order and IDs:
  `vscode.openFolder`, then `git.cloneRecursive`. The command registry owns the
  runtime routing; this SCM model only publishes the stable action contract.
- **The upstream `git.missing`, `git.parentRepositoryCount`,
  `git.unsafeRepositoryCount`, and `git.closedRepositoryCount` context keys are
  not read.** `BuildGitScmWelcomeModel` takes the explicit
  `EGitScmWelcomeWorkspaceState` (`Empty`, `Folder`, `WorkspaceWithFolders`, or
  `WorkspaceWithoutFolders`) plus repository presence. The richer upstream
  gates fold into `EGitScmWelcomeContent::None` when a provider is present,
  rather than being approximated by a partial read of keys this product does
  not publish.
- **No progress indicator, and `git clone` still runs to completion or
  cancellation without a queue.** This mirrors the remote-commands and
  branch-commands divergences below: there is no operation queue and no
  `operationInProgress` context key here yet. The clone's own responsiveness
  comes from `RunGitCloneExecute` taking a `HANDLE stop` and being callable
  off the UI thread, not from a shared queue.
- **`git.defaultCloneDirectory` is not read.** `GitCloneOptions` hard-codes
  upstream's `recurseSubmodules` default (`false`); the parent directory always
  comes from `browseForParentDirectory`, matching upstream's behavior when the
  setting is unset. Revisit alongside the other `git.*` settings recorded
  elsewhere in this file as not yet readable from configuration.
- **The welcome content's `<a href="command:...">` Markdown links become plain
  hit-testable button rectangles.** Upstream's `viewsWelcome` body is rendered
  Markdown, and the action is an inline link styled as a monaco button by CSS.
  `CScmWorkbenchTool`'s native owner-drawn list host has no Markdown or HTML
  renderer, so `LayoutWelcome`/`PaintWelcome` draw the same two
  facts — a message and a set of labeled, clickable actions — as GDI `RoundRect`
  buttons instead, hit-tested and dispatched by `WelcomeSegmentIndexAt` /
  `InvokeWelcomeSegmentAt`. This mirrors the repository band's own
  "painted by the tool, not by the list" divergence recorded above rather than
  inventing a second interaction model.
- **The welcome message's Markdown formatting is flattened to plain
  word-wrapped text.** Upstream's welcome body can carry inline emphasis and
  other Markdown spans; `PaintWelcome` measures and draws the message with a
  single `DrawTextW(DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX)` call
  and one text colour (`palette.primaryText`). The message's *content* is
  upstream's own string from `BuildGitScmWelcomeModel`; only its typographic
  styling is simplified, the same trade-off the commit-message prompt's caption
  substitution above already makes for a different upstream surface.
- **The welcome content uses a native GDI implementation of ViewWelcome's
  flow.** `LayoutWelcome` starts one `em` below the view body, gives each
  direct child a one-`em` block-start margin, centers the content column, caps
  it at 300 DIP, and makes each action button span that column. Markdown is
  still flattened to plain text and owner-drawn buttons (see the preceding
  divergence), but the top-flow geometry no longer vertically centers the
  block.
- **The numbers behind that flow are owned by `workbench/ViewsWelcomeMetrics.h`
  (2026-08-20, #218).** This view and the Explorer render the same upstream
  `viewsWelcome` contribution, and each used to own its own copy of the
  geometry, so the two drew visibly different buttons. Two defects were on this
  side: the box height came from the label font (`textHeight + 2 * 6`), which
  both disagreed with the Explorer and moved with the UI font, and
  `PaintWelcome` filled the `RoundRect` through a `NULL_PEN`, which fills one
  pixel short of the laid-out rectangle. Upstream's `.monaco-text-button` is
  `box-sizing: border-box` with `line-height: 16px`, `padding: 4px 8px`, and a
  `1px` border -- a font-independent 26 DIP -- and its `background-color`
  covers the whole border box, so the fill now uses a pen of the fill colour.
  `LayoutWelcome`/`PaintWelcome` read `views::WelcomeButtonHeight`,
  `WelcomeButtonCornerRadius`, `WelcomeHorizontalInset`, and
  `WelcomeButtonColumnWidth`; do not restore a local `kWelcome*` constant here.
- **Corrected record — `SetHasOpenFolder` now has a production caller, so both
  welcome states are reachable.** An earlier version of this entry recorded that
  `CScmWorkbenchTool::Impl::hasOpenFolder` stayed `false` forever, pinning the
  view to `EmptyWorkbench` (Clone-only) and making `FolderNoRepository` (Init)
  dead code. `CEditWnd` now calls `SetHasOpenFolder(!root.empty())` at exactly
  the two places it already calls `SetRoot(root)` — `InitializeWorkbench()` and
  `ApplySemanticWorkspaceContext()` — where `root` is
  `CEditWnd::GetSemanticWorkspaceRoot()`. That accessor returns a non-empty path
  only when `CWorkbenchRuntime`'s workspace snapshot is `Folder` with exactly one
  folder, which is the state this flag is defined against. Deriving both from the
  same expression is the point: the root the view lists files under and the
  welcome branch it shows when there is no repository cannot disagree, and a
  multi-root workspace correctly reports neither a root nor an open folder.

**Current contract superseding that historical record:** the production API is
`SetWelcomeWorkspaceState`, not `SetHasOpenFolder`. It projects the explicit
`Empty`, `Folder`, `WorkspaceWithFolders`, and `WorkspaceWithoutFolders` states;
the paragraph uses the full body width with 20 DIP side insets, while only the
centered action-control column is capped at 300 DIP. The empty-workbench
actions are `vscode.openFolder` followed by `git.cloneRecursive`; the folder
variant uses `git.init?[true]`, and workspace-without-folders uses
`workbench.action.addRootFolder`.

### Wiring `git.init` and `git.clone` into the Command Registry

**Corrected record:** the entry below described a gap that has since been
closed. `git.init` and `git.clone` are now registered in
`WorkbenchCommandRegistry.cpp`'s `RegisterGitCommands` batch through a new
`MakeGitAlwaysAvailableDescriptor(id, title)` factory — distinct from
`MakeGitDescriptor`, which every other entry in that batch uses — and
`CScmWorkbenchTool` now renders the empty-state welcome content and dispatches
both commands from it. What was recorded as future work is now the shipped
shape:

- Both commands carry a `when` clause of `"workbenchReady"`, not
  `"gitOpenRepositoryCount != 0"` the way the rest of `RegisterGitCommands`'s
  entries do — `git.init` and `git.clone` are precisely the commands that must
  stay available when there is **no** open Git repository, which is the
  opposite condition the other remote/branch/commit/stage commands require.
  `MakeGitAlwaysAvailableDescriptor` sets both `whenClause` and
  `enablementClause` to `"workbenchReady"` for exactly this reason; using
  `MakeGitDescriptor` here would have been backwards.
- Both are in the same atomic `RegisterGitCommands` batch as the remote,
  branch, commit, and stage commands, under upstream's own IDs `git.init` and
  `git.clone`, with the Command Palette titles `Git: Initialize Repository`
  and `Git: Clone`, matching upstream's category-prefixed Command Palette
  convention already used for every other entry in that batch. `git.init` is
  registered as a `WorkbenchCommandArgumentExecutor`
  (`WorkbenchGitCommandExecutors::init`) because the welcome content's
  `Initialize Repository` button dispatches it with the `[true]`
  `skipFolderPrompt` argument documented above; `git.clone` is a plain
  `WorkbenchCommandExecutor` (`WorkbenchGitCommandExecutors::clone`) because
  upstream's own handler takes no argument.
- `CScmWorkbenchTool`'s welcome content now renders `BuildGitScmWelcomeModel`'s
  message and action buttons natively and dispatches each button's `command`/
  `argumentsJson` through the same `CommandCallback` the repository band and
  resource-row commands already use (`InvokeWelcomeSegmentAt` →
  `Impl::runCommand`), so a click on `Initialize Repository` or
  `Clone Repository` reaches the registry entries above end-to-end. Coverage
  for the registry wiring itself lives in a new suite,
  `GitInitCloneCommandRegistry.*`
  (`src/test/cpp/tests1/workbench/GitInitCloneCommandRegistryTest.cpp`), kept
  separate from `WorkbenchCommandRegistryTest.cpp` only because that file was
  outside this pass's editable scope, not because the two commands need a
  different contract from the rest of the batch.

## Divergences

Each entry states the constraint and the chosen behavior. An undocumented
divergence is a bug.

- **Retracted: "Resource click opens the file, not a diff."** That entry recorded
  the absence of a diff editor and said it would end with slice 3. It has. The
  published resource command is now upstream's own `resolveChangeCommand`
  result — `vscode.diff` when both sides resolve, `vscode.open` when only the
  right-hand side does — and `git.openChange` is registered and routed. The
  remaining diff-side divergences are in "Opening a Change" above.
- **`ScmResourceState::contextValue` is left empty for built-in Git resources.**
  Upstream's getter returns its `_repositoryKind`. **Updated record:** resource
  context menus have now landed, and they still do not consume `contextValue`:
  the built-in Git menus are selected by the row's *group*, exactly as upstream's
  `when` clauses select them by `scmResourceGroup`. There is no additional
  consumer for `contextValue`; publishing an invented value would still be
  worse than publishing none.
- **`git.alwaysShowStagedChangesResourceGroup` is not read.** Its documented
  default (`false`) is hard-coded into the `index` group's `hideWhenEmpty`.
  Hard-coding the upstream default keeps the empty state identical to a stock VS
  Code; inventing a third behavior would not. Revisit when the setting is
  readable from configuration.
- **`CommandCallback` reports recognition, not success.** `CEditWnd` now installs
  one; it forwards to `TryExecuteWorkbenchStableCommand` and returns that
  method's `handled` flag, which is true only once
  `WorkbenchCommandRegistry::Find` matched the id. Returning recognition rather
  than `void` is what preserves the `git.openFile` route below: an unregistered
  id falls back to the file-activation callback, exactly as it did before a
  callback existed, while `git.checkout` reaches the real registry. A callback
  that swallowed unimplemented command IDs silently would be worse than none.
- **Resolved: branch commands use the shared chromeless Quick Input overlay.**
  Checkout and branch creation no longer open `CQuickInputDialog`. Their
  continuation flow returns to the editor message loop between decisions, and
  the window-lifetime session gate makes cancellation and teardown explicit
  terminal paths.
- The checkout picker renders upstream's `branches` / `remote branches` / `tags`
  separators as non-selectable group headings, paints `$(codicon)` label runs,
  filters by typed text, and switches command-row ordering through
  `BuildCheckoutItems(..., filterIsEmpty)` just as the model specifies. Stable
  item identities map accepted rows back to their `GitCheckoutItem`; list indexes
  are never treated as Git ref identities.
- Branch-name entry is another mode of the same overlay. A colliding name
  reopens that mode with the validation message and preserves the rejected value;
  the accepted empty value remains cancellation. No nested message loop or
  captioned native dialog is involved.
- **Branch-command messages go to the status bar, not to a notification.**
  There is no native notification producer, and adding one here would create a
  second notification authority.
  The status bar is the same surface the colour-theme and file-icon-theme pickers
  already report through. Revisit when a native notification source exists.
- **The commands run `RunGit` synchronously on the UI thread with no progress
  indicator.** Upstream runs them through its operation queue and reports
  progress in the SCM view, and `enablement: !operationInProgress` disables a
  command while another is running. Neither the queue nor that context key exists
  here yet, so the window blocks for the duration of one bounded, timeout-guarded
  git invocation instead of pretending to be asynchronous.
- **The `when` clause is `gitOpenRepositoryCount != 0` alone.** Upstream's is
  `config.git.enabled && !git.missing && gitOpenRepositoryCount != 0`. The other
  two are not keys this product publishes: there is no `git.enabled` setting to
  read and no `git.missing` probe, and a clause referencing an unpublished key
  would evaluate false and hide the commands entirely.
  `gitOpenRepositoryCount` is owned by the core context projection rather than by
  an extension, because our Git provider is native.
- **Retracted: "`git.sync` and `git.publish` are deliberately not registered."**
  That entry recorded the absence of a remote-command route and said it would end
  with slice 4. It has. All nine remote commands are registered with real
  executors; see "Remote Commands" above for what they do and where they still
  diverge.
- **Upstream's branch-related settings are hard-coded to their documented
  defaults.** `git.branchPrefix` (empty), `git.branchWhitespaceChar` (`-`),
  `git.branchValidationRegex` (empty), `git.branchProtection` (none),
  `git.checkoutType` (all three groups), `git.showReferenceDetails` (`true`),
  `git.commitShortHashLength` (`7`), and `git.pullBeforeCheckout` (`false`) are
  not read from configuration. Hard-coding the upstream default keeps behavior
  identical to a stock VS Code; inventing a third behavior would not. Revisit
  when these settings are readable.
- **Both SCM status items share one `status.scm` visibility ID, and only the
  first provider's commands are shown.** Upstream gives each contributed status
  command its own entry and shows every provider's. The native status bar's
  hidden-item model is keyed by stable ID, so the branch item and the sync item
  are one hideable unit here. Inventing per-item IDs VS Code does not publish
  would break the customization menu's identity contract; a second provider's
  commands are absent rather than merged into the first provider's row. Revisit
  when a provider actually publishes a second set.

## Running git

`RunGit` is the only way this subsystem starts a git process. It is bounded and
cancellable, and its terminal state is typed.

- Terminal states are distinct on purpose: `GitUnavailable`, `LaunchFailed`,
  `TimedOut`, `Cancelled`, `OutputLimitExceeded`, `InvalidRequest`, `Failed`,
  `Succeeded`. An empty change list means "clean" **only** when the refresh
  reported `Succeeded`; otherwise it means "unknown", and `CScmWorkbenchTool`
  keeps `execution` and `failureReason` separate from `state` so the two can
  never be confused.
- The child environment sets `GIT_TERMINAL_PROMPT=0`. Without it a credential
  prompt makes fetch/pull/push hang forever and surface only as a timeout, which
  names the wrong cause.
- `BuildEffectiveGitArguments` prepends `-C <workingDirectory>`. The working
  directory alone is not enough: a directory can sit inside a different
  repository's worktree, and `-C` is what makes git resolve the repository the
  request names.
- Git executable discovery uses the shared Windows executable resolver. PATH is
  parsed explicitly, empty and relative entries are ignored, and the selected
  `git.exe` path is absolute; process CWD is never an executable search root.
- Status and history use the typed `PassiveRepositoryRead` policy. The runner
  injects `-c core.fsmonitor=false` and rejects a caller-side fsmonitor
  override, so opening or refreshing a repository cannot execute a
  repository-configured fsmonitor command. Ordinary user-invoked Git commands
  retain normal Git configuration behavior. This is an explicit interim
  divergence from VS Code: upstream disables its Git extension in untrusted
  workspaces, while this product has no Workspace Trust authority yet, so the
  narrower fsmonitor restriction applies to every passive refresh. Revisit the
  policy when a real workspace-trust boundary exists.
- Child inheritance is an explicit standard-stream handle list, and the Git
  process is assigned atomically at creation to a kill-on-close job. Timeout,
  cancellation, and output-limit terminal paths therefore own descendant
  cleanup without exposing unrelated inheritable editor handles.
- stdin is written on its own thread while the parent drains stdout and stderr.
  A commit message can exceed the pipe buffer, and a blocking parent write would
  deadlock against a child waiting for its stdout to be drained.
- Argument quoting follows `CommandLineToArgvW`, including the trailing
  backslash-run rule. Branch names, paths, and commit messages all reach git
  through it.

## Git Output Channel

`GitOutputChannel.h`/`.cpp` (Issue #221) is an adapter, not a new authority: it
mirrors `RunGit` invocations into `workbench::output::OutputService` as the
"Git" Log channel, exactly as upstream's built-in Git extension mirrors its own
`_exec` calls into its own "Git" Output channel. `OutputService` itself remains
the sole owner of channel content; nothing here retains its own copy.

- **Format verified against `microsoft/vscode` source, not guessed.** The
  built-in Git extension creates its channel with
  `window.createOutputChannel('Git', { log: true })` in
  `extensions/git/src/main.ts` (a `LogOutputChannel`, i.e. this codebase's
  `EOutputChannelKind::Log`, not `Output`). `extensions/git/src/git.ts`'s
  `_exec` always logs `` > git ${args.join(' ')} [${elapsed}ms] `` and logs
  stderr only when it is non-empty; it never logs stdout or an exit-code line.
  `main.ts`'s log listener splits each logged string on `\r?\n`, drops trailing
  blank lines, and rejoins the remainder with `\n` before one
  `LogOutputChannel.appendLine` call, which itself resolves to Info level
  (`ExtHostLogOutputChannel.appendLine` delegates to `info()`). `kGitOutputChannelId`
  reuses `GitScmPublisher::kGitProviderId` ("git") because `createOutputChannel`
  itself carries no separate stable channel ID upstream publishes; `kGitOutputChannelLabel`
  is upstream's own display string, "Git", verbatim.
- **Two deliberate, documented divergences**, both in `GitOutputChannel.h`'s
  `BuildGitOutputLogEntries` doc comment: the logged command line uses this
  runner's own *effective* arguments (`BuildEffectiveGitArguments`, which
  includes the leading `-C <workingDirectory>`) rather than upstream's raw
  `args.join(' ')`, since this runner's repository resolution is `-C`-based and
  upstream passes `cwd` out of band; and stdout is never logged, which is not a
  divergence in outcome — upstream's own `git.commandsToLog` default is `[]`, so
  a stock VS Code never logs stdout either, and this adapter has no settings
  reader for that list yet.
- **`EnsureGitOutputChannel` is Snapshot-based, not replay-cache-based.**
  `OutputService::CreateChannel` called twice for an already-created channel
  with two *different* `operationId`s is a `Conflict`/`InvalidChannelId` by
  design (see `ValidateOwnedChannel`/`CreateChannel` in `OutputService.cpp`),
  and the remembered-operation replay cache is bounded
  (`maximumRememberedOperations`, default 512) and can evict the original
  create operation over a long-lived owner generation. `EnsureGitOutputChannel`
  therefore checks `OutputService::Snapshot()` for an existing channel with the
  same `channelId`/owner/kind first, and only calls `CreateChannel` when none is
  found.
- **`RunGitLogged` never changes `RunGit`'s own result.** Output-channel
  mirroring is strictly best-effort: a null `OutputService*` in `GitOutputSink`,
  an exhausted `nextAppendOperationId` callable, or any non-`Conflict` failure
  from `EnsureGitOutputChannel` all skip logging silently and still return
  `RunGit`'s result unchanged.
- **Thread safety matches `RunGit`'s own.** `RunGitLogged` carries no shared
  mutable state beyond the `OutputService` (already documented thread-safe) and
  the caller-supplied `HANDLE stop`/callables, so it is safe to call from the UI
  thread (as `CEditWnd.cpp`'s existing `RunGit` call sites already do) or from a
  background worker thread (as `CScmWorkbenchTool.cpp`'s periodic status-refresh
  thread already does), provided each call supplies its own `stop` handle and
  its own `nextAppendOperationId` sequence.
- **Not yet wired into any production call site.** `CScmWorkbenchTool.cpp` and
  `CEditWnd.cpp`'s existing `RunGit(...)` calls are unchanged; replacing them
  with `RunGitLogged(...)` and threading a `GitOutputSink` through is future
  work, out of scope for this adapter-only change.

## Verification

- `build-sln.bat x64 Debug`, then the focused filter
  `tests1.exe --gtest_filter=GitCommandRunner.*:GitScmModel.*:GitScmPublisher.*:GitScmMenus.*:GitRefModel.*:GitBranchCommands.*:GitStageCommands.*:GitCommitCommands.*:GitDiffModel.*:GitLineStaging.*:GitSyncCommands.*:GitInitCloneCommands.*:GitInitCloneCommandRegistry.*`.
  Add `ApiCommandArguments.*:WorkbenchCommandRegistry.*:WorkbenchContextKeyService.*:WorkbenchWhenClauseEvaluator.*`
  when the command registration or `when` clause changes; that cohort passed
  295/295 across 15 suites on 2026-08-06 with no surviving `tests1` or
  repository-built `sakura` process. It passed 232/232 earlier the same day,
  before the remote commands, 194/194 before line staging, 141/141 before the
  diff model, and 102/102 on 2026-08-05, before the commit commands. Neither
  the `GitInitCloneCommands.*` suite added by Issue #27 nor the new
  `GitInitCloneCommandRegistry.*` suite covering the `git.init`/`git.clone`
  registry wiring and `CScmWorkbenchTool`'s welcome-content rendering has been
  run through this filter yet — this pass wrote the source and tests but did
  not build, per this pass's own prohibitions; run the widened filter before
  relying on the new pass/fail count. `CScmWorkbenchTool`'s own
  `LayoutWelcome`/`PaintWelcome`/`WelcomeSegmentIndexAt`/
  `InvokeWelcomeSegmentAt` methods have no dedicated test of their own — there
  is no existing precedent in this repository for testing that HWND-owning
  class's paint/hit-test logic in isolation (the repository band's equivalent
  `PaintBand`/`SegmentIndexAt` methods are likewise untested directly), so
  coverage stops at the pure `BuildGitScmWelcomeModel` decision function
  (`GitInitCloneCommandsTest.cpp`) and the registry wiring
  (`GitInitCloneCommandRegistryTest.cpp`) above it. A future pass adding native
  UI test infrastructure for this window should close that gap rather than
  inventing a one-off harness here.
- `ApiCommandArguments.*` was silently outside the focused filter until the diff
  work widened it, so its tests built and linked without ever running in a
  focused pass. Widen the filter with the suite name whenever a new suite lands;
  a suite the filter does not name is indistinguishable from a suite that passes.
- The publisher tests read the two compared sides back out of the published
  URIs, through `ParseApiDiffArguments` / `ParseApiOpenArguments` and
  `ResolveGitDiffEndpointUri`, rather than comparing URI strings. The assertion
  is about *which two texts a click compares*; a string comparison would instead
  pin down how `toGitUri` happens to spell them.
- The rebase/merge tests create a throwaway directory under
  `std::filesystem::temp_directory_path()`, because `ReadInProgressState` probes
  `MERGE_HEAD` and the rebase state directories with `GetFileAttributesW` and no
  injected callable can intercept a real filesystem call.
- `sakura.vcxproj` deletes `x64\Debug\sakura.exe` before linking, so a running
  editor fails the build with `MSB3073` even when every translation unit
  compiled. Close the running editor rather than assuming a compile error.
- The changed-file list draws its own VS Code-style overlay scrollbar through the
  shared `workbench/controls/COverlayScrollbar`, the same control the Explorer
  tree uses, so the two views cannot drift apart visually. The LISTBOX keeps
  `WS_VSCROLL` because the overlay reads the target's `SB_VERT` `SCROLLINFO` as
  the authoritative scroll state; the native bars are only hidden. Scrolling goes
  back through one callback that sends `LB_SETTOPINDEX`. Refresh the overlay
  (`UpdateListScrollbar`) after anything that changes item count, size, or top
  index -- `Populate`, `LayoutList`, `SetPalette`, and the list subclass's
  `WM_VSCROLL`/`WM_MOUSEWHEEL`/`WM_KEYDOWN`/`WM_SIZE` handling all do.
- Row icons resolve through the bundled `vs-seti` theme with
  `icons::seti::ResolveSetiFileIcon`, painted by the shared
  `icons/SetiIconPainter.h` that the Explorer also uses, so a file shows the same
  glyph in both views. The light/dark variant comes from
  `theme::CThemeService::IsActiveColorThemeLightKind()`, and `kInheritColor`
  means the row's own text color. Only when the Seti font is unavailable does the
  row fall back to the generic `file` codicon; group header rows keep the chevron.

## The Commit Action Button (2026-08-20)

`ISCMProvider.actionButton` is what upstream's `SCMViewPane` renders as a split
button directly under the commit box, and the built-in Git extension is what
contributes it. The model is `BuildGitCommitActionButton` in `GitScmMenus`; the
native half is `CScmWorkbenchTool`'s `ActionButton*` members, and the band it
occupies is `ScmViewStackLayout::actionButton`.

- The button exists only while the repository has at least one resource in some
  group, because that is upstream's own gate. With nothing to commit upstream
  contributes **no** button, so this shows none rather than a disabled one. Its
  band collapses with it, so the change list keeps the room it had before.
- `enabled` follows the commit box. Upstream disables the whole button while a
  repository operation runs, which is the same condition that disables the box,
  so reading a second authority here could only make the two disagree.
- The box is `.monaco-text-button`, so its height and corner radius come from
  `workbench/ViewsWelcomeMetrics.h` -- the same header the ViewWelcome buttons
  read. A local constant here would let the two disagree about one upstream
  control, which is exactly the defect #218 fixed for the Explorer.
- The primary half runs `git.commit`; the dropdown half opens
  `secondaryCommands` and runs the chosen id and arguments through the same `runCommand`
  route. The title is kept in `renderLabelWithIcons` syntax (`$(check) Commit`)
  so the native renderer draws upstream's own Codicon.

Recorded divergences (omit, don't fake):

- **`Commit & Push` and `Commit & Sync` use a native payload projection.**
  Upstream passes its `SourceControl` object and the post-commit command in
  `git.commit`'s arguments. The native boundary already owns the repository,
  so the action button publishes `[]`, `["git.push"]`, or `["git.sync"]`; the
  composition root commits first and then calls the existing push/sync
  executor. The SCM refresh is asynchronous, so the immediate sync path raises
  the known-ahead count to at least one after a successful commit; it does not
  infer a remote or invent a second Git implementation.
- **`Commit (Signed Off)` is absent** for the same reason: `git.commitSignedOff`
  is not registered here.
- **The button is always the commit button.** Upstream's action button is a
  state machine that also becomes `Publish Branch` or `Sync Changes` when there
  is nothing to commit but something to publish or sync. Those states need the
  branch's publish/ahead-behind conditions wired into the button model; until
  they are, no button appears in that state rather than a commit button that
  would commit nothing.

## The lists scroll the wheel themselves (2026-08-20, #227)

Both list boxes keep `WS_VSCROLL` so their `SCROLLINFO` stays authoritative for
the themed overlay scrollbar, but the overlay hides the platform bar — and a
list box whose scroll bar is hidden drops `WM_MOUSEWHEEL` on the floor. Verified
2026-08-20: `LB_SETTOPINDEX` moved both lists, while a `WM_MOUSEWHEEL` sent
straight to either list box left `LB_GETTOPINDEX` at 0.

`ListSubclassProc` therefore scrolls explicitly through `ScrollListBoxByWheel`
before `DefSubclassProc`, exactly as the Explorer tree already did, and then
republishes the overlay's extent. It honours `SPI_GETWHEELSCROLLLINES`,
including `WHEEL_PAGESCROLL`, so the wheel moves what the system says a notch
moves rather than a hard-coded row count.

This is only half of the path: the wheel still has to reach the list. See
`window/CLAUDE.md`, "The wheel follows the pointer, not the focus", for the
frame-side routing that delivers it to the hovered control instead of the
focused one.

## Git file decorations feed the Explorer, not the SCM view (2026-08-20, #229)

VS Code separates the SCM service from `IDecorationsService`: the File Explorer
paints `FileDecoration` values published by a provider, and it never sees a
`SourceControlResourceState`. This product keeps that boundary.

- `workbench/decorations/FileDecorationModel.h` is the provider-neutral model.
  It carries a badge, a tooltip, a theme-color *role* (`EFileDecorationColor`),
  and upstream's `propagate` flag. A provider never resolves a COLORREF and a
  consumer never learns which provider decorated a path.
- `GitFileStatusDecorationColor` reproduces the Git extension's own
  `Resource.getStatusColor` mapping, and `DoesGitFileStatusPropagate` its
  `resourceDecoration.propagate = type !== DELETED && type !== INDEX_DELETED`.
  Both take `EGitFileStatus`, which is why `GitResourceDecoration` carries the
  status rather than a resolved color: the SCM row and the Explorer row derive
  the same facts from the same value.
- `BuildGitFileDecorationEntries` is the projection onto native paths. A
  resource whose URI is not a file URI is dropped, not guessed at.
- `CScmWorkbenchTool` publishes the whole table on every render, the way
  upstream's `onDidChangeFileDecorations` carries a set rather than a delta.
  `RepublishFileDecorations` republishes the last built set without re-running
  git, which is what a settings change needs.

Recorded omissions (omit, don't fake):

- **`gitDecoration.ignoredResourceForeground` is registered but never
  published.** Upstream's `GitIgnoreDecorationProvider` runs `git check-ignore`
  over the resources the Explorer asks about; that query does not exist here, so
  no path is decorated as ignored. The role stays in the model and the theme so
  that adding the provider is the only remaining work.
- **Submodule decorations are not published** for the same reason: the repository
  model does not enumerate submodules, so nothing can produce the `S` badge.
