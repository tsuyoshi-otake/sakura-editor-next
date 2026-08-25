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

## Single-line input projection

The 26-DIP input token describes the painted Quick Input frame, not the native
single-line EDIT HWND. USER32 anchors the caret near the top when a single-line
EDIT is stretched to that full height. Keep the EDIT inset by three DIP above
and below (and one DIP inside each horizontal border), while the parent paints
the complete 26-DIP background and focus frame. Tests must protect both
rectangles at 96, 120, 144, and 192 DPI; changing only the outer frame does not
verify caret alignment.

## Dismissal and repaint transaction

Quick Input is non-modal but loses its session when a mouse button is pressed
outside the overlay. Cancel and hide it before dispatching that press, then leave
the message unconsumed so the clicked workbench target still receives the user's
action. Presses inside the input, list, and close button remain part of Quick
Input and must not dismiss it through this path.

Filtering replaces the item model and the owner-drawn LISTBOX as one paint
transaction. Disable LISTBOX redraw around `LB_RESETCONTENT` / `LB_ADDSTRING`,
finish selection and layout, synchronously paint the final subtree, and only
then publish the selection callback. A callback is allowed to change the theme;
running it against the previous overlay bounds creates a visible intermediate
frame and is therefore outside the contract.
