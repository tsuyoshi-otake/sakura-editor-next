# Native Markdown preview compatibility

The acceptance target is VS Code's Markdown preview behavior where it can be
implemented safely by Sakura's lightweight native renderer. The production path
contains no embedded browser, script runtime, remote renderer, or runtime asset
download.

## Current compatibility contract

| Area | Native contract | Status |
|---|---|---|
| Headings, paragraphs, emphasis, strong, inline code | Typed blocks/spans, native measurement and paint | Implemented |
| Lists, block quotes, rules, fenced code | Typed blocks with stable source-line ranges | Implemented |
| Tables | Native rows/cells/alignment; no HTML execution | Implemented |
| Links and images | Typed URI disposition; unsafe schemes fail closed | Partial: activation remains gated |
| Raw HTML | Harmless semantic wrappers only; active content and attributes never execute | Safe subset |
| Live update | Persistent worker; one in flight plus latest pending; stale generations discarded | Implemented |
| Scroll synchronization | Editor line maps to preview block and preview position scrolls source without moving the caret | Implemented |
| Preview lock | Dynamic follows active Markdown; locked retains source identity | Implemented state boundary |
| Commands/keybindings | Exact VS Code IDs and default Markdown preview bindings | Implemented |
| Math and diagrams | Inert literal source with an explicit native-fallback notice; never executed | Safe fallback |

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

## Verification checklist

1. Command registry: every ID resolves to a distinct executor outcome.
   Verify: focused command tests. Expect: exact IDs and keybindings.
2. Preview state: show/toggle/reopen/lock/refresh and document-switch branches
   terminate explicitly. Verify: pure state-model tests. Expect: no accidental
   terminal intermediate state.
3. Renderer: safe representative Markdown and malicious HTML/URI samples.
   Verify: parser/window model tests. Expect: supported structures render and
   active content remains blocked.
4. Update/scroll UX: repeated revisions coalesce and source mappings clamp.
   Verify: pure update/mapping tests. Expect: latest revision renders once and
   both mapping directions remain within document bounds.
5. Dependency boundary: source/project/package search.
   Verify: search for embedded-browser and script-runtime dependencies. Expect:
   no production dependency or bundled script asset.
