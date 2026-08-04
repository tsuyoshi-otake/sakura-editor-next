/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/outline/OutlineViewLifecycle.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::outline {

//! Immutable input captured on the editor thread for built-in Outline parsing.
//!
//! The object is populated before it is published as a shared_ptr<const ...>.
//! Nothing in this value refers back to CEditDoc, CDocLine, CLayout, a window, or
//! process-global settings.  Logical positions are returned by the worker;
//! native layout/control work remains on the editor thread.
struct OutlineDocumentSnapshot final {
	struct LineSpan final {
		std::size_t offset = 0;
		std::size_t length = 0;
	};

	OutlineDocumentVersion documentVersion{};
	std::wstring filePath;
	std::wstring textWithEol;
	std::vector<LineSpan> lineSpans;
	bool extendedLineDelimiters = false;
	// These small presentation values are copied on the UI thread.  The
	// snapshot parser must not query LS()/shared settings while it is running.
	std::wstring cppAnonymousName;
	std::wstring cppDefinitionPosition;
	std::wstring javaDefinitionPosition;
	std::map<int, std::wstring> appendText;

	//! Construction helper used before the snapshot is published as const.
	//! Returning false makes size overflow a terminal capture failure instead of
	//! allowing an offset/length table to describe memory outside the buffer.
	[[nodiscard]] bool AppendLine( std::wstring_view line )
	{
		// Keep the same NUL-terminated line contract as CDocLine while retaining
		// one contiguous allocation.  The separator is not part of span.length.
		const std::size_t remaining = textWithEol.max_size() - textWithEol.size();
		if( line.size() >= remaining ) return false; // includes the separator
		if( lineSpans.size() == lineSpans.max_size() ) return false;
		const auto offset = textWithEol.size();
		if( !line.empty() ) textWithEol.append(line.data(), line.size());
		textWithEol.push_back(L'\0');
		lineSpans.push_back({ offset, line.size() });
		return true;
	}

	[[nodiscard]] bool IsValid() const noexcept
	{
		if( !documentVersion.IsValid()
			|| lineSpans.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ) return false;
		const std::size_t textSize = textWithEol.size();
		for( const auto& span : lineSpans ) {
			// Written as subtraction so offset + length cannot wrap.  A separator
			// must be present even for an empty logical line.
			if( span.length > static_cast<std::size_t>((std::numeric_limits<int>::max)())
				|| span.offset >= textSize
				|| span.length >= textSize - span.offset
				|| textWithEol[span.offset + span.length] != L'\0' ) return false;
		}
		return true;
	}

	[[nodiscard]] int LineCount() const noexcept
	{
		return lineSpans.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
			? static_cast<int>(lineSpans.size()) : 0;
	}
};

} // namespace workbench::outline
