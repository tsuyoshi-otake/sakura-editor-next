# Search View Guidance

## Scope

This directory owns the workspace search/replace authority
(`WorkspaceSearchEngine`), its pure result model (`SearchModel.h`), and the
native Search view (`CSearchWorkbenchTool`). The tracking Issue is #237.

The surface is modelled on `vs/workbench/contrib/search/browser/searchView.ts`
and `searchWidget.ts`, read as source rather than inferred from a screenshot.

## Identifiers and Placement

- The ViewContainer is `workbench.view.search` and the View is `workbench.view.search`
  in upstream's registry; `WorkbenchIds` carries both, `BuiltinPartProjection`
  maps them through `kSideBarLocations`, and `ActivityBarEntryProjection` renders
  the entry. Search therefore lives in the **Primary Side Bar** by default and can
  be moved to the Secondary Side Bar exactly as Explorer and Source Control can.
- `CViewContainerPages` owns the single physical `CSearchWorkbenchTool` HWND for
  the container, the same ownership rule Explorer and SCM follow. The tool owns no
  resource ids: `ApplySearchTexts()` resolves every localized string at the
  composition layer and hands them over as one `SearchViewTexts` value.

## The Widget

`searchWidget.ts`'s controls are reproduced with upstream's own semantics:

- The query box carries the three toggles `Match Case`, `Whole Word`, and
  `Use Regular Expression`, and the replace row is opened by the same chevron
  that upstream puts to the left of the two boxes.
- Results are grouped by file, one header row per file with its match rows below,
  and a row is activated by a single click.
- `Replace All`, per-file replace, and single-match replace are the three replace
  operations, matching upstream's row-level, file-level, and global actions.
- Regular expressions run through `CBregexp` (bregonig). `InitRegexp` is called
  with `bShowMessage = false` because the engine runs on the worker thread and a
  message box off the UI thread would be a second, unowned window.

## Activation Goes Through the Marker Path

Activating a result calls `CEditWnd::OpenDocumentAtMarkerPosition`, which is the
same function the Problems panel activates a marker with. Search records 1-based
UTF-16 line/column and `OpenSearchMatch` converts to the zero-based form that
path takes. There is deliberately only one activation path: a marker and a search
match must never land differently.

## Divergences

Each entry states the constraint and the chosen behavior.

- **Activation places the caret; it does not select the match.** Upstream reveals
  the match and selects its range. The shared activation path takes a single
  position, and widening it to a range is the same UTF-16-range adapter the
  Problems panel is still waiting for (see [`../problems/CLAUDE.md`](../problems/CLAUDE.md)).
  Selecting an approximated range would be worse than placing the caret exactly.
- **`files to include` / `files to exclude` are absent.** Upstream's two glob
  inputs need a glob matcher and the `search.exclude` / `files.exclude` settings,
  none of which is readable here. The inputs are omitted rather than rendered
  inert.
- **Search history is not persisted.** Upstream stores the query, replace text,
  and toggle state in its view-state memento. There is no memento key for this
  view yet, so all of it resets with the window — the same gap the SCM view's pane
  sizes have.
- **There is no Search Editor** (`search.action.openNewEditor`) and no
  `Open in editor` link. That is a separate editor input, not a variant of this
  view.
- **`workbench.view.search` and `workbench.action.replaceInFiles` are not
  registered in `WorkbenchCommandRegistry`**, so there is no Command Palette entry
  and no `Ctrl+Shift+F` / `Ctrl+Shift+H` keybinding. The Activity Bar entry reveals
  the container through `ActivateSidebarPage`, which is the same route Source
  Control already takes. Registering them additionally needs a new legacy function
  code in `Funccode_x.hsrc`; do that as one change with SCM rather than giving
  Search a private route.
- **Replace rewrites the file on disk through the `.skrnew` temp-file pattern**,
  re-encoding with the file's detected `CCodeBase` and preserving its BOM. Upstream
  edits through its text-model layer, so a file already open in another editor
  process is not updated in place here; it is rewritten and the other window sees
  it as an external change.
- **The result set is bounded** and the view says so through
  `STR_WORKBENCH_SEARCH_LIMIT_HIT`. Upstream pages; a bounded scan keeps the worker
  cost fixed and the bound is reported rather than hidden.
