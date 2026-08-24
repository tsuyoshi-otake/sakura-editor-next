# Quick Input Compatibility Boundary

## VS Code reference

The native Command Palette targets VS Code 1.134.0 Quick Input geometry: a
62%-of-host width capped at 600 CSS pixels, `6px 6px 4px` header padding, a
26px input, 6px list side padding, 7px list bottom padding, a 12px outer corner
radius, a 6px input radius, and a 3px selected-row radius.

## Native HWND divergence

`CCommandPaletteOverlay` is an ordinary child HWND so that Quick Input stays
non-modal and participates in the editor's existing focus, cancel, and command
terminal paths. An opaque child cannot paint VS Code's `shadow-xl` outside its
own client rectangle while preserving the pixels of arbitrary editor content
behind the shadow. Until the workbench has a layered sibling/compositor surface,
the native projection uses a two-DIP inner elevation edge inside the 12-DIP
rounded region. A fake rectangular external band is not an acceptable substitute.

The Command Palette also remains locked to the `>` provider because this fork
does not yet implement VS Code's general Quick Open provider switch. Removing
the marker normalizes it back into the EDIT while filtering receives the text
after the marker. When Quick Open exists, replace this boundary with real
provider switching rather than retaining the normalization behavior.
