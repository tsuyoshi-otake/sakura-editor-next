/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/unicode/TerminalGraphemeWidth.h"

#include <algorithm>

// Keep the pinned MIT implementation private to this adapter.
#include "terminal/vendor/windows_terminal/src/types/inc/CodepointWidthDetector.hpp"

namespace terminal::unicode {

GraphemeMeasurement MeasureFirstGrapheme(std::wstring_view text) noexcept
{
	if (text.empty()) return {};
	GraphemeState state{};
	CodepointWidthDetector::Singleton().GraphemeNext(state, text);
	return {
		static_cast<std::size_t>(std::max(0, state.len)),
		std::clamp(state.width, 0, 2),
	};
}

} // namespace terminal::unicode
