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

**Divergence (#264):** because the provider is locked, the caret is *pinned*
after the `>` marker -- `Home`, `Shift+Home`, a click in the marker's cell, and
a drag that starts left of it all clamp the selection to offset one. Upstream
lets the caret reach offset zero precisely so the user can delete `>` and switch
providers; here that would only produce the normalization loop above, which
appended a stray marker on every edit before the clamp existed. Remove the clamp
in the same change that adds provider switching, not before. The clamp runs
*after* `DefSubclassProc`, because `PreTranslateMessage` sees a key before the
EDIT has moved the caret and a mouse-placed caret raises no `EN_CHANGE`.

## Single-line input projection

The 26-DIP input token describes the painted Quick Input frame, not the native
single-line EDIT HWND. USER32 top-anchors a single-line EDIT's formatting
rectangle inside a client taller than one text line, and `EM_SETRECT` is
documented as multiline-only, so a full-height EDIT paints the caret near the
top no matter what padding the parent reserves.

The fix is not padding. Measure the control font's `TEXTMETRICW::tmHeight`,
size the EDIT client to exactly that one line, and centre that HWND inside the
painted frame -- `workbench/controls/CInputBoxGeometry`'s
`CenterSingleLineEditor` performs that calculation for every workbench input, so
Quick Input and Search cannot drift apart. `ComputeQuickInputRowGeometry` takes
the measured line height as its last argument; zero means "not measured yet" and
falls back to the CSS three-DIP inset, which is the only case that inset still
describes. Cache the measurement per DPI and re-measure when the font is
recreated.

Tests must protect both rectangles at 96, 120, 144, and 192 DPI, and must assert
the *editor* height equals the measured line height. Changing only the outer
frame, or asserting a symmetric inset around an over-tall EDIT, does not verify
caret alignment: at 96 DPI a 20px EDIT holding a 13px line measures 3px above
the caret and 10px below it while every inset assertion still passes (#263).

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
