/*! @file
	@brief Vertical geometry for the workbench's native VS Code input boxes

	In VS Code, Search and Quick Input are the very same component:
	`vs/base/browser/ui/inputbox/inputBox.ts`'s `InputBox`, reached through
	`ContextScopedHistoryInputBox` and `QuickInputBox` respectively. The Source
	Control commit box is deliberately different even upstream -- `SCMInputWidget`
	hosts a Monaco `CodeEditorWidget` -- because it is a multi-line editor.

	Neither of those layers contains any centering code: a DOM `<input>` and a
	Monaco line box are centered by CSS `line-height` for free. Win32 has no
	equivalent, so the workbench has to supply that missing layer itself. This
	file is that layer. It exists so the substitute is written once instead of
	being copied into every view, which is what let the Search box and the
	Command Palette box drift high while the Source Control box was correct.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>

namespace workbench::controls {

/*!
	@brief Centers a single-line EDIT's client inside its painted input frame.

	A single-line Win32 EDIT top-anchors its formatting rectangle inside a client
	that is taller than one text line, and `EM_SETRECT`/`EM_SETRECTNP` are
	documented as multi-line only, so that rectangle cannot be moved. Sizing the
	control to exactly one text line and centering the HWND is therefore the only
	way to place the caret where CSS would put it. Insetting the HWND by a fixed
	padding centers the window while leaving the text high, which is the defect
	this function replaces.

	@param frame               the painted input box, in the parent's coordinates
	@param lineHeight          measured `TEXTMETRICW::tmHeight` of the input font
	@param horizontalInset     pixels to keep inside the painted vertical borders
	@param fallbackLineHeight  used when `lineHeight` has not been measured yet
*/
[[nodiscard]] constexpr RECT CenterSingleLineEditor(
	const RECT& frame,
	int lineHeight,
	int horizontalInset,
	int fallbackLineHeight) noexcept
{
	const int frameWidth = static_cast<int>(frame.right - frame.left);
	const int frameHeight = (std::max)(0, static_cast<int>(frame.bottom - frame.top));
	const int inset = (std::min)((std::max)(0, horizontalInset), (std::max)(0, frameWidth) / 2);
	const int requested = lineHeight > 0 ? lineHeight : fallbackLineHeight;
	const int editorHeight = (std::min)((std::max)(1, requested), frameHeight);
	const int top = frame.top + (frameHeight - editorHeight) / 2;
	return RECT{
		frame.left + inset,
		top,
		(std::max)(frame.left + inset, frame.right - inset),
		top + editorHeight,
	};
}

/*!
	@brief Measures one text line of `font` in `owner`'s device context.

	Returns `TEXTMETRICW::tmHeight`, or zero when the measurement is unavailable
	(no window, no DC). Callers cache the result per DPI; a zero result means
	"not measured yet" and must select a fallback rather than being stored.
*/
[[nodiscard]] int MeasureTextLineHeight(HWND owner, HFONT font) noexcept;

} // namespace workbench::controls
