# Native ViewContainer and View projection

- `IViewContainerPage`/`ViewContainerPagePool` owns the one-page-per-container
  mounting contract. A physical Part hosts a page; it is never a View identity.
- Dynamic page publication uses `ViewContainerPageRegistry::PrepareBatch` and
  `Commit`. Preparation owns every allocation and captures the registry revision;
  commit is one non-throwing swap and rejects stale, foreign, or reused batches.
  This is the native half of a larger catalog/page publication transaction.
- `CViewContainerPages::PrepareContributedPages` applies the same fence to its
  canonical contribution and page-id sets. Before `Create` it stages startup
  factories without advertising pages; afterward it appends the live registry
  and all Part-facing IDs in one allocation-free commit.
- `ProjectHostViewPages` groups Views only when their product-owned provider
  supplies `factoryForContainer`. It calls that factory once per container and
  publishes one page descriptor. A singleton `factory` still rejects a second
  View, and mixed providers in one container fail closed. This distinction keeps
  the ViewContainer/View layers intact instead of fabricating one page per View.
- `CSenpViewContainers` prepares one generation of native containers and Views
  hidden before publication. Every View requires a real native body factory;
  missing providers, malformed batches and unsupported destinations fail closed.
  Packages never receive factories, callbacks, HWNDs or UI Automation providers.
- A View retains its body, collapse and preferred size when hidden or moved
  between supported containers. Closing an old container cannot destroy a View
  moved elsewhere. Owner Close revokes callbacks before destroying all bodies;
  retained factories and focus tokens cannot recreate that generation.
- `ApplyLayout` consumes committed placement/visibility snapshots. It validates
  every owned View before native movement and compensates changed parents on
  failure. Failed compensation closes the generation. A missing native body or
  unexpected window destruction makes the whole projection unusable.
- The body owns its descendants and discards `interactionChanged` on Close.
  Report descendant focus and mouse entry/exit changes so the projection can
  coalesce one posted refresh. There are no polling timers or host I/O here.
- A fatal native body failure calls `projectionFailed`; this marks the cohort
  unusable without deleting the body during its own callback. Discard this
  callback on Close too. A provider's ordinary loading/error state is not a
  native projection failure.
- `ViewPaneLayout` bounds the stack to 64 panes; layout distributes surplus in
  O(N), preserves minimum extents, and scrolls the container when they exceed
  available height. Even a viewport shorter than a header must have valid scroll
  bounds. Collapse, visibility, native focus and owner revocation remain distinct.
- `ViewPaneChrome` is shared with SCM. The verified VS Code header is 22 DIP,
  with a 16-DIP twistie and 2-DIP side margins, 11-DIP uppercase title, and
  2-DIP action padding / 4-DIP gap / 8-DIP trailing margin. CJK thread UI
  languages use normal weight; other languages use bold. Do not revive SCM's
  previous 30-DIP header as a second standard.
- A native header owns its accessibility lifetime. Invalidate it and call
  `UiaReturnRawElementProvider(hwnd, 0, 0, nullptr)` at WM_DESTROY; reject
  WM_GETOBJECT during revocation. Invalidation alone does not clear Windows'
  HWND/event map and failed the combined UIA test despite passing in isolation.
- Current supported placement is within this owner cohort. A product-owned or
  foreign cohort page without a retained-View transfer contract returns
  `Unsupported`. The pure catalog can describe a wider destination; that alone
  is not native support. Cross-cohort transfer and fallback require a complete
  publication/retention transaction before enabling that capability.
- The current projection captures collapse/size through `Snapshot` and accepts
  restored values at construction; it performs no durable writes. Profile
  restoration and dynamic package publication must be wired by native
  composition (U06). The v2 package gate remains UnsupportedRuntime until the
  body, command, persistence and tool adapters exist, so these incomplete user
  capabilities are not presented as working VS Code features.

Run the focused `SenpViewContainer`, `ViewPaneStackLayout`, existing page/layout,
SCM and `CustomUiAutomationProvider` tests after a solution build. Use
`tools/verify-senp-view-rendering.ps1` for the separately enabled visual probe;
ordinary tests never keep a window waiting for an external driver. Screen-vs-
PrintWindow evidence must include a same-geometry full-redraw noise floor and
prove both the gesture and process cleanup. The probe's stock EDIT body is a
test fixture, not a shipped Tree View or document provider.
