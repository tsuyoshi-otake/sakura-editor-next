# Native Markdown preview compatibility

The acceptance target is VS Code's Markdown preview behavior where it can be
implemented safely by Sakura's lightweight native renderer. The production path
contains no embedded browser, script runtime, remote renderer, or runtime
rendering-asset download. VS Code Strict-compatible HTTPS image responses are
the only remote content admitted, through a bounded anonymous native loader.

## Current compatibility contract

| Area | Native contract | Status |
|---|---|---|
| Headings, paragraphs, emphasis, strong, inline code | Typed blocks/spans, native measurement and paint | Implemented |
| Lists, block quotes, rules, fenced code | Typed blocks with stable source-line ranges | Implemented |
| Tables | Native rows/cells/alignment; no HTML execution | Implemented |
| Links and images | Typed URI disposition; local images and Strict HTTPS image responses render; unsafe schemes, non-image responses, and insecure redirects fail closed | Partial: link activation remains gated |
| Raw HTML | Allowlisted harmless wrappers projected to native blocks/spans; active content and attributes never execute | Safe subset (explicit allowlist) |
| Live update | Persistent worker; one in flight plus latest pending; stale generations discarded | Implemented |
| Live width resize | Transient native geometry per pointer sample; one committed Markdown reflow on mouse-up; explicit rollback on cancellation | Implemented and frame-measured |
| Scroll synchronization: editor to preview | `scrollPreviewWithEditor`. The editor's top layout line maps to the first rendered row at or after it | Implemented and measured |
| Scroll synchronization: preview to editor | `scrollEditorWithPreview`. The preview's top row scrolls the source view without moving the caret | Implemented and measured |
| Preview lock | Dynamic follows active Markdown; locked retains source identity | Implemented state boundary |
| Commands/keybindings | Exact VS Code IDs and default Markdown preview bindings, compared against a pinned upstream manifest | Implemented; to-side commands are typed unsupported |
| Typography | Font family, sizes, weights, line heights and body padding taken from upstream `media/markdown.css`; ClearType on every face | Implemented; per-element block margins and the h1/h2 rule are not drawn |
| Math | Inert literal source with an explicit native-fallback notice; never executed | Deliberate divergence: `mathTypesetting` stays `Unsupported` |
| Mermaid | The flowchart subset is parsed and drawn natively; every other family, and every flowchart feature outside the subset, stays inert literal source with a notice | `mermaidFlowchartRendering` is `Supported`; `mermaidNonFlowchartRendering` stays `Unsupported` |

Unsupported syntax must remain visible as literal text or a typed unsupported
state. It must not be approximated with unrelated editor state merely to resemble
a screenshot.

## Command behavior

The registered commands are:

- `markdown.showPreview`
- `markdown.showPreviewToSide`
- `markdown.showLockedPreviewToSide`
- `markdown.showSource`
- `markdown.showPreviewSecuritySelector`
- `markdown.preview.refresh`
- `markdown.preview.toggleLock`
- `markdown.reopenAsPreview`
- `markdown.reopenAsSource`
- `markdown.togglePreview`

The default bindings are `Ctrl+K V` for `markdown.showPreviewToSide` and
`Shift+Ctrl+V` for `markdown.togglePreview`.

Sakura currently owns one real EditorGroup. `showPreviewToSide` and
`showLockedPreviewToSide` therefore register under their exact upstream IDs but
return a typed unsupported result. They never alias to Sakura's legacy sibling
preview pane. Current-group show/reopen/toggle owns replacement layout; the
legacy function-code path alone owns `NativeSiblingPane` placement.

`markdown.showPreviewSecuritySelector` remains a typed unsupported command.
The native renderer implements the default `Strict` level, including HTTPS
images, but cannot honestly implement upstream's `Disable` option because it has
no browser script runtime. Showing a visually complete selector would therefore
declare behavior that this host cannot provide.

The document tab's preview button is therefore **not** a `markdown.*` command. It
toggles the Sakura-owned sibling pane, so its declared action id is
`sakura.toggleMarkdownSiblingPreview` (`tabbar::kMarkdownPreviewCommandId`),
dispatched as `F_TOGGLE_MARKDOWN_PREVIEW` into
`MarkdownPreviewCommandState::ToggleNativeSibling`. It becomes
`markdown.showPreviewToSide` only when a real second EditorGroup exists; naming
it that today would declare a capability this shell does not have.

## Declared capabilities

`markdown::PreviewCapabilities` is the machine-readable form of this table. Scroll
synchronization is declared per direction, named after the upstream settings that
gate it, because one combined flag cannot state that one direction works and the
other does not:

| Capability | Value | Upstream setting |
|---|---|---|
| `localImageProjection` | Supported | - |
| `secureRemoteImageProjection` | Supported | Strict `img-src ... https: data:` policy |
| `linkActivation` | Unsupported | - |
| `scrollPreviewWithEditor` | Supported | `markdown.preview.scrollPreviewWithEditor` |
| `scrollEditorWithPreview` | Supported | `markdown.preview.scrollEditorWithPreview` |
| `rawHtmlExecution` | Unsupported | - |
| `mathTypesetting` | Unsupported | - |
| `mermaidFlowchartRendering` | Supported | - |
| `mermaidNonFlowchartRendering` | Unsupported | - |

The `Unsupported` rows are deliberate divergences, not gaps awaiting a patch.
They must stay fail-closed and covered by tests that assert the boundary.

## Upstream comparison point

`upstream-parity-manifest.json` pins the VS Code commit this contract was
compared against, together with the 13 preview settings, the 10 preview commands,
and the 2 default keybindings extracted from it. Regenerate and diff that file to
detect upstream drift instead of maintaining a hand-written list. Sakura does not
yet honor the preview settings; the manifest records what upstream declares, not
what this shell implements.

## Verification checklist

Each item names the test that actually runs it. An item with no executable check
is not verified, however obviously correct the code looks.

1. Command registry: every ID resolves to a distinct executor outcome, and no
   command opens a preview for a non-Markdown or unnamed source.
   Verify: `MarkdownPreviewCommandStateTest.cpp`. Expect: the to-side commands
   stay `UnsupportedSideEditorGroup` and never report `NativeSiblingPane`.
2. Document tab button: the declared action id names the concept it dispatches.
   Verify: `MarkdownPreviewCommandStateTest.TheDocumentTabButtonDispatchesTheSakuraSiblingToggle`.
   Expect: `sakura.toggleMarkdownSiblingPreview` toggling the native sibling pane.
3. Preview state: show/toggle/reopen/lock/refresh and document-switch branches
   terminate explicitly. Verify: `MarkdownPreviewCommandStateTest.cpp`. Expect: no
   accidental terminal intermediate state.
4. Async update: one in flight plus one latest pending, stale results never
   publish, `Closed` is terminal, and a failed delivery never revives a
   generation. Verify: `MarkdownPreviewAsyncStateTest.cpp`, which replays every
   sequence of six transitions over the full alphabet rather than a hand-picked
   scenario list. Expect: the invariants hold after every transition.
5. Scroll mapping: both directions clamp inside the document and round-trip
   without drift. Verify: `MarkdownPreviewScrollMapTest.cpp`. Expect: an empty
   layout maps neither direction, and an out-of-range position clamps.
6. Scroll synchronization, end to end: verified empirically on the running
   editor, because a pure mapping test cannot prove the two panes are wired.
   Verify: drive `F_GOFILETOP` / `F_GOFILEEND`, preview wheel/key input, and the
   explicit overlay thumb under a throwaway profile. Read editor `SCROLLINFO`,
   preview geometry, and the painted overlay endpoint; preview `SCROLLINFO` must
   remain absent. Expect: both panes move once without a feedback loop.
   Measured 2026-08-20, x64 Debug, 120-section document, profile `mdscrollsync`:
   editor 0 -> 446 moved the preview 12 -> 7431; preview 12 -> 1812 moved the
   editor 0 -> 106. That historical run predated the explicit model and read the
   preview's then-native `SCROLLINFO`; process and profile cleanup were verified.
7. Live resize and painted-frame integrity: User32-driven alternating divider
   samples on a throwaway profile. Verify:
   `tools/measure-markdown-preview-resize.ps1`. Expect: preview screen-versus-
   `PrintWindow` difference at or below its redraw noise floor, no
   `WS_VSCROLL`, exactly one aligned overlay, cancellation restores committed
   width, overlay destruction is complete, and no run-owned process/profile
   remains. Measured 2026-08-22, x64 Debug, 60 samples in a 480 by 240 window:
   `CLAUDE.md` median 1.904 ms, p95 12.832 ms, maximum 16.681 ms, committed
   reflow 32.700 ms; the 1,249,037-byte/5,604-line synthetic startup sample
   median 1.833 ms, p95 11.723 ms, maximum 13.222 ms, committed reflow
   536.140 ms. Both runs had preview difference 0/60, native scrollbar leaks
   0/60, one aligned overlay, successful cancellation/lifecycle probes, and
   verified process/profile cleanup.
8. Renderer: safe representative Markdown and malicious HTML/URI samples.
   Verify: `MarkdownParserTest.cpp`, `MarkdownPreviewLayoutTest.cpp`. Expect:
   supported structures render and active content remains blocked.
   Measured 2026-08-23 with the x64 Debug editor and a User32-driven fixture:
   `<br>`, semantic inline wrappers, `<details>`, `<dl>`, a captioned table,
   and `<pre>` content were visible in the native preview; script/style/form
   content was absent. Four divider samples had preview `PrintWindow` versus
   screen difference 0/4, no `WS_VSCROLL`, and no run-owned process survivors.
9. Declared capabilities: the typed boundary matches this table.
   Verify: `MarkdownParserTest.ExposesUnsupportedNativeCapabilitiesAsTypedBoundaries`.
   Expect: math and Mermaid stay `Unsupported`.
10. Dependency boundary: source/project/package search.
   Verify: search for embedded-browser and script-runtime dependencies. Expect:
   no production dependency or bundled script asset.
11. Strict remote images: parser URL disposition plus fake request-service
    responses. Verify: `MarkdownParserTest.cpp` and
    `MarkdownRemoteImageFetcherTest.cpp`. Expect: HTTPS image responses load;
    HTTP, user-info, bad final URLs, non-image content, non-200 responses, and
    cancellation terminate without publishing image bytes or opening a network
    connection in tests.

Not yet verified, and therefore not claimed anywhere above: the preview settings
in `upstream-parity-manifest.json` are not read, and there is no differential
conformance check against a pinned markdown-it oracle.

## Typography

The preview reproduces VS Code's preview text rendering rather than the editor's
own document typography. The values are copied from
`extensions/markdown-language-features/media/markdown.css` at the commit pinned
in `upstream-parity-manifest.json`, together with the defaults of
`markdown.preview.fontSize` (14) and `markdown.preview.lineHeight` (1.6).

| Property | Upstream | Native preview |
|---|---|---|
| Prose face | `-apple-system, BlinkMacSystemFont, "Segoe WPC", "Segoe UI", system-ui, ...` | `Segoe UI` (the Windows end of the stack) |
| Code face | `var(--vscode-editor-font-family)` | The editor's document font, falling back to Consolas |
| Base size | 14px | 14 DIP |
| Body line height | 22px (14 * 1.6) | 22 DIP, with the measured text height as a floor |
| Code line height | 1.357em | 19 DIP |
| Heading sizes | 2 / 1.5 / 1.25 / 1 / 0.875 / 0.85 em | The same ladder, in per-mille |
| Heading weight | 600 | `FW_SEMIBOLD` |
| Heading line height | 1.25 | 1.25 of the heading size |
| Heading margins | 24px top, 16px bottom, h1 top 0 | The same, with CSS margin collapsing |
| Body padding | `0 26px`, `padding-top: 1em` | 26 DIP sides, 14 DIP top |
| Antialiasing | DirectWrite subpixel | `CLEARTYPE_QUALITY` on every created face |

Not reproduced, and therefore not claimed: per-element block margins other than
headings (paragraphs, lists, block quotes and tables share one 1em gap), the
h1/h2 bottom border with `padding-bottom: 0.3em`, and `th/td { padding: 5px 10px }`.
`markdown.preview.fontFamily`, `fontSize` and `lineHeight` are not read from
settings yet; the defaults are compiled in.
