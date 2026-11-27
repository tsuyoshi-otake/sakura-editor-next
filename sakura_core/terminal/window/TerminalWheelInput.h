/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <cmath>
#include <cstddef>

namespace terminal {

//! Per-pane wheel remainder for the alternate-scroll conversion, mirroring
//! xterm.js `MouseService._wheelPartialScroll` (xterm.js @ da9919caf,
//! src/browser/services/MouseService.ts L478-480). Owned by the pane.
struct TerminalWheelAccumulator {
	double partial{};
};

//! One WM_MOUSEWHEEL message after the units xterm.js sees: the raw delta,
//! the modifier state, and the host settings needed to turn it into rows.
struct TerminalWheelScrollInput {
	int delta{};
	bool shift{};
	//! Alt or Ctrl: xterm.js multiplies by fastScrollSensitivity for either
	//! (MouseService.ts L445-451); Shift is rejected before this is read.
	bool fastModifier{};
	//! SPI_GETWHEELSCROLLLINES. Chromium maps WHEEL_PAGESCROLL to a DOM page
	//! delta and multiplies a value of 0 into the pixel amount the same way
	//! (ui/events/blink/web_input_event_builders_win.cc L299-335).
	unsigned long linesPerNotch{ 3 };
	//! Cell height in physical pixels, paired with paneDpi below. Chromium
	//! delivers the wheel delta in CSS pixels: it multiplies the native
	//! delta by the device scale factor and WheelEvent divides it back out
	//! by devicePixelRatio (blink_event_util.cc L557-570, wheel_event.cc
	//! delta_y_ / dpr), so the amount reaching xterm.js is scale-free.
	//! xterm.js then divides by cellHeight / dpr (MouseService.ts L467-471):
	//! a CSS-pixel row height, equal to rowHeightPx * 96 / paneDpi.
	double rowHeightPx{};
	//! Pane DPI; the wheel math must use the CSS row height, never the
	//! physical one.
	double paneDpi{ 96.0 };
	//! Viewport row count, used only for the WHEEL_PAGESCROLL page mode.
	std::size_t bufferRows{};
	//! VS Code defaults: mouseWheelScrollSensitivity 1, fastScrollSensitivity 5.
	double scrollSensitivity{ 1.0 };
	double fastScrollSensitivity{ 5.0 };
};

//! Whole rows a single wheel message contributes to the alternate-scroll
//! conversion. Zero means nothing is sent. The caller emits at most one
//! cursor key per message regardless of the returned magnitude
//! (MouseService.ts L281-292) and derives the direction from the delta sign.
[[nodiscard]] inline int TerminalConsumeWheel(
	TerminalWheelAccumulator& accumulator, const TerminalWheelScrollInput& input ) noexcept
{
	// _consumeWheelEvent rejects non-vertical and Shift-held events first.
	if( input.delta == 0 || input.shift ) return 0;
	const double sensitivity = input.fastModifier
		? input.fastScrollSensitivity * input.scrollSensitivity
		: input.scrollSensitivity;
	const double ticks = static_cast<double>(input.delta) / WHEEL_DELTA;
	if( input.linesPerNotch == WHEEL_PAGESCROLL ) {
		// Chromium emits a DOM_DELTA_PAGE delta of +/-ticks; xterm.js answers
		// ticks * sensitivity * rows without accumulating (MouseService.ts
		// L481-483) and _handlePassiveWheel sends a key for any non-zero
		// amount, so a sub-row page still yields a signed non-zero result.
		const double pageAmount = ticks * sensitivity * static_cast<double>(input.bufferRows);
		const int wholeRows = static_cast<int>(pageAmount);
		if( wholeRows != 0 ) return wholeRows;
		return pageAmount > 0.0 ? 1 : (pageAmount < 0.0 ? -1 : 0);
	}
	// Chromium emits deltaY = ticks * linesPerNotch * (100/3) pixels
	// (kScrollbarPixelsPerLine); a lines-per-notch of 0 therefore yields 0.
	const double pixels = ticks * static_cast<double>(input.linesPerNotch) * (100.0 / 3.0);
	if( pixels == 0.0 || input.rowHeightPx <= 0.0 || input.paneDpi <= 0.0 ) return 0;
	const double cssRowHeight = input.rowHeightPx * 96.0 / input.paneDpi;
	double amount = pixels * sensitivity / cssRowHeight;
	if( std::fabs(pixels) < 50.0 ) amount *= 0.3; // likely-trackpad damping (L473-476)
	accumulator.partial += amount;
	const int lines = static_cast<int>(accumulator.partial); // truncates toward zero
	accumulator.partial -= lines;
	return lines;
}

} // namespace terminal
