# P4 Editor View Adapter Guidance

Editing views render and interact with the active editor pane selected by the
workbench. They must tolerate no active editor without constructing a fake
document or exposing a backing `CEditDoc` as open state.

Keep logical focus separate from Win32 focus. Keyboard-only navigation, screen
reader names/roles/states, high contrast, zoom, selection, hover, caret, and
100/125/150/200 percent DPI behavior are acceptance requirements. Painting and
layout consume theme/layout tokens rather than duplicating visual constants.

The minimap is a navigation surface, not a hover-preview surface. Its idle
timer must not create a floating `CTipWnd` containing a four-line source
excerpt; `CTipWnd` remains reserved for explicit keyword-help behavior in the
main editor. This keeps the minimap free of an unrelated yellow overlay while
preserving keyboard and pointer navigation.
