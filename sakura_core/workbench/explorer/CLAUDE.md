# Explorer View Guidance

## Scope

`CExplorerTool` is the native Files Explorer view (Win32 TreeView, worker-thread
enumeration, overlay scrollbar). `ExplorerContextMenuModel.h` is its HWND-free
context-menu row model. Command identity, surface bindings, and the wire payload
live in [`../commands/CLAUDE.md`](../commands/CLAUDE.md); this file owns the
Explorer surface's own behavior and its recorded divergences from VS Code.

## Context Menu

- `BuildExplorerContextMenuRows` is the only authority for the menu's contents
  and order. It reproduces upstream `MenuId.ExplorerContext` sorting —
  `navigation` first, remaining groups lexicographic, one separator between
  adjacent non-empty groups — restricted to the eight commands
  `RegisterExplorerCommands` implements. The native projection renders rows and
  resolves titles/keybinding hints from the command registry; it must not
  reorder, insert, or retitle items.
- The model emits command IDs only. Titles are the registry's facts; a second
  title table here would drift.
- A workspace root gets no rename/deletion rows (upstream negates them via
  `ExplorerRootContext`), and the trash/permanent-deletion split follows
  upstream's two separate menu items on `ExplorerResourceMoveableToTrash` —
  never one item reading a flag.

## Recorded Divergences (omit, don't fake)

Upstream shows more in this menu than this product implements. Unimplemented
items are **omitted entirely**, never rendered disabled or approximated with
legacy behavior:

- `navigation`: Open to Side (`explorer.openToSide`), Open With...
  (`explorer.openWith`).
- `2_workspace`: Add Folder to Workspace / Remove Folder from Workspace on
  roots (multi-root workspace management is not implemented).
- `3_compare`: Compare with Selected / Select for Compare / Compare Selected.
- `5_cutcopypaste`: Cut / Copy / Paste.
- `5b_importexport`: Download / Upload (web/remote-only upstream).
- Multi-select: the native Explorer has no multi-select, so no menu item acts
  on a resource list; the command payload contract rejects upstream's optional
  second (multi-select) argument rather than acting on its first element.

Each of these must appear in its upstream group/order when implemented; adding
one means extending the model and its tests, not patching the projection.

- **Writable precondition is not yet modeled.** Upstream grays New File/New
  Folder/Rename/Paste through `ExplorerResourceWritableContext` (a non-writable
  filesystem provider). This provider serves the local Win32 filesystem only,
  which registers as writable, so the model has no `writable` input yet. Add
  the input when a read-only provider exists; do not pre-invent a context key
  that can never be false.

## File-Operation Executors (2026-08-13, #146)

The eight commands' native executors live in `CEditWnd` (`ExecuteExplorer*` /
`CommitExplorerRename` / `CommitExplorerCreate`), writing through a
window-owned lazy `platform::filesystem::IFileService`. The confirmation
wording model is `ExplorerDeleteConfirmation.h` and the relative-label/name
validity model is `ExplorerResourcePath.h`; both are pure and header-only so
their tests transcribe upstream wording/rules instead of a native dialog.

Recorded divergences of this flow (omit, don't fake):

- **Delete confirmation has no "Do not ask me again" checkbox.** Upstream's
  checkbox writes `explorer.confirmDelete`; until that setting is read and
  written here, every delete asks. A checkbox that remembered nothing would be
  the faked capability.
- **Permanent-delete file detail reads "This action is irreversible!"** instead
  of upstream's file-only "You can restore this file using the Undo command.",
  because this product has no file-operation Undo. The folder wording is
  upstream's own.
- **Operation failures surface as a modal error dialog.** Upstream reports
  them through the notification center; this product's notification surface
  cannot carry these yet, so the report is modal rather than silently logged.
- **An invalid entered name is no commit.** Upstream's inline rename box shows
  a live validation message under the input; the native TreeView label editor
  has no message surface, so a name failing `IsValidExplorerEntryName` simply
  does not commit and the edit ends with the entry unchanged.

## View Title and Native Row Projection (2026-08-16, #178)

- Explorer is a View in the Primary Side Bar, not a separate Part. Its native
  30-DIP header uses the workspace folder's display label with the filesystem's
  original casing; it never uppercases a local path.
- The four header controls route through upstream's real `MenuId.ViewTitle`
  command IDs: `workbench.files.action.createFileFromExplorer`,
  `workbench.files.action.createFolderFromExplorer`,
  `workbench.files.action.refreshFilesExplorer`, and
  `workbench.files.action.collapseExplorerFolders`. They resolve the active
  TreeView selection/root in `CExplorerTool`; the command registry remains the
  execution authority.
- Tree rows are 22 DIP and use native disclosure buttons without the legacy
  dotted TreeView connector lines. A transparent native image slot preserves
  the TreeView geometry while the bundled icon theme paints each row. File rows
  draw Seti, VS Code's own default file icon theme; see the icon section below.
- With no root, the TreeView is hidden and the view projects the locally
  representable `EmptyView` variants from upstream's
  `explorerViewlet.ts`: `NoFolder` has the distinct `No Folder Opened` View
  title, then renders `You have not yet opened a folder.`, `Open Folder`
  (`workbench.action.files.openFolder`), `You can clone a repository locally.`,
  and `Clone Repository` (`git.clone`) in one ordered welcome model;
  `NoFolderWithEditors` adds the upstream explanation and `Add Folder`
  (`workbench.action.addRootFolder`); and an empty workspace renders
  `You have not yet added a folder to the workspace.` plus `Add Folder to
  Workspace` (`workbench.action.addRootFolder`). These are real registered
  commands, never disabled placeholders.
- The native `ViewWelcome` flow uses upstream's `views.css` geometry: 20-DIP
  horizontal inset, one-em top-flow gaps, a full-width wrapped paragraph, and
  only the action buttons capped at 300 DIP and centered. The content therefore
  starts at the top of the view body instead of being vertically centered.
- **`Open Remote Repository` is intentionally not projected.** In VS Code it
  is contributed by a Remote Repositories provider and opens a virtual remote
  workspace without cloning. Sakura has neither that provider contract nor a
  remote/virtual filesystem, so presenting a button that clones locally or
  fails after a click would fake the capability. A future implementation must
  add the provider and virtual-workspace boundary first; only then may its
  conditional ViewWelcome contribution appear.
- **Multi-root Explorer remains an explicit unsupported boundary.** A saved
  workspace with one or more folders cannot be collapsed to a fabricated single
  TreeView root, so the tool shows `WorkspaceWithFoldersUnsupported` with no
  action instead of falsely saying that no folder is open. A future real
  multi-root tree model must replace this state; it must not map it to the
  no-folder welcome content.

### File icons are the real `vs-seti` theme (2026-08-19, #196)

VS Code selects the `vs-seti` icon theme when the user has chosen none, so this
product bundles the same artwork and the same association data as a first-party
built-in rather than approximating it. `sakura_core/workbench/icons/` owns the
payload, the generator, and the upstream pin; `SETI-ATTRIBUTION.md` there records
the commit, the hashes, and the MIT notice. Read that file before changing what
an Explorer row draws.

- `ResolveSetiFileIcon` is the only file-icon authority for a row, and it
  reproduces upstream's CSS-specificity order: whole file name, then the longest
  dotted extension suffix, then the theme default `file`. Do not add a local
  association, retitle a glyph, or re-colour one; the table is generated data.
- **Folder rows draw no icon, because the theme contributes none.** `vs-seti`
  has no `folder`, `folderExpanded`, `rootFolder`, or `rootFolderExpanded` key,
  so upstream leaves `hasFolderIcons` false and a folder row is its twistie and
  its name. Substituting a Codicon folder glyph here would look more finished
  and would not be VS Code's default theme.
- Seti icons are coloured, and the colour is the theme's, not the view's. The
  one exception is upstream's `_todo` definition, which carries no `fontColor`;
  those rows inherit the Explorer text colour through `kInheritColor`.
- The light colour set is used for exactly `ColorThemeKind.Light`. Upstream
  emits the theme's `light` section under the `.vs` body class alone and Seti
  contributes no `highContrast` section, so High Contrast **and** High Contrast
  Light both keep the base colours. The row reads
  `CThemeService::IsActiveColorThemeLightKind()`, which the composition root
  publishes from the loaded theme's own `ColorThemeKind`. It is not a palette
  colour and never the window's saved dark/light mode, so do not derive it from
  `ExplorerPalette` or from how bright a background looks.

Recorded divergence of this flow (omit, don't fake):

- **`workbench.iconTheme` is not a setting yet.** VS Code lets the user pick
  another file icon theme or `null` for none; this product has no icon-theme
  registry and no extension host to contribute one, so Seti is fixed. Add the
  setting when a second theme actually exists; a setting with one value would
  pretend at a choice.
- **The language layer is folded in at generation time.** Upstream's third
  association layer matches a `languageId` through its language registry, which
  this product does not have. The generator folds each Seti-known language into
  the extensions and file names that language claims in VS Code's own built-in
  extensions, weakest layer first, so a directly named key still wins. Without
  the fold the theme would not resolve `.cpp`, `.h`, `.md`, `.json`, or `.py` at
  all.
- **Codicons remain the fallback, and only as a fallback.** If the embedded
  `seti.ttf` fails to register, `PaintNodeIcon` uses the first-party Codicon
  association table in `ExplorerFileIcon.h` instead of drawing Seti code points
  in whatever face GDI substitutes. That path is a degraded picture, not the
  intended one; do not extend the Codicon table to close a gap in Seti.
