/*! @file
 * @brief Pure VS Code Position/Range -> Sakura CLogicPoint conversion.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "basis/SakuraBasis.h"
#include "workbench/problems/MarkerService.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace workbench::problems {

//! Returns one document line's content in Sakura's native storage unit
//! (`wchar_t`, i.e. one UTF-16 code unit per element), with any EOL sequence
//! and any byte-order mark already stripped -- exactly what
//! `CDocLineMgr`/`CDocLine` hold once a file has been loaded. Called with a
//! zero-based line index that has already been clamped to
//! `[0, totalLineCount)`, so an implementation never has to guard its own
//! bounds. This is the only document-dependent seam in this file: everything
//! else here is a pure function of its arguments.
using LogicLineContentLookup = std::function<std::wstring_view(std::uint32_t zeroBasedLine)>;

//! Reports which axis, if any, of a converted position had to be moved off
//! the value the caller asked for. Neither flag is an error by itself --
//! see `ConvertMarkerPositionToLogicPoint` for why clamping is the chosen
//! behavior -- but a caller that wants to warn the user (for example "the
//! diagnostic's line no longer exists") can act on it.
struct MarkerPositionClamp final {
	bool line = false;
	bool column = false;

	[[nodiscard]] bool Any() const noexcept { return line || column; }
	[[nodiscard]] bool operator==(const MarkerPositionClamp&) const noexcept = default;
};

struct ConvertedLogicPosition final {
	CLogicPoint position;
	MarkerPositionClamp clamp;
};

struct ConvertedLogicRange final {
	ConvertedLogicPosition start;
	ConvertedLogicPosition end;
};

//! Converts one zero-based VS Code `Position` (line, UTF-16-code-unit
//! `character`) to a Sakura `CLogicPoint` (zero-based logic line/column).
//!
//! Sakura's logic column is already counted in the same unit as a VS Code
//! `Position.character`: `CDocLine` stores a line as a `wchar_t` array (one
//! element per UTF-16 code unit, see `sakura_core/doc/CLAUDE.md` and
//! `CDocLine::GetLengthWithEOL`), and `CLogicInt`/`CLogicPoint` count logic
//! positions in that same array (`sakura_core/basis/SakuraBasis.h`;
//! `CViewCommander::Command_TagJumpNoMessage` and
//! `CEditWnd`'s `MYWM_SETCARETPOS` handler both set/read a `CLogicPoint`
//! straight from an external 1-based line/column pair with only a -1
//! offset, no unit rescale). A surrogate pair is therefore two logic units
//! here, exactly as it is two UTF-16 code units in a VS Code `Position` and
//! two `wchar_t` elements in a `CDocLine` -- this function does not, and
//! must not, convert to Unicode code points or grapheme clusters.
//!
//! Tab expansion is deliberately out of scope. Sakura's logic coordinates
//! are pre-tab-expansion by construction (that is the entire distinction
//! between `CLogicPoint` and `CLayoutPoint`; see
//! `CLayoutMgr::LogicToLayout`), so a converted `CLogicPoint` already needs
//! no tab-width adjustment -- the caller runs it through the existing
//! `LogicToLayout` conversion the same way a tag-jump landing position does.
//!
//! `totalLineCount` and `lineContent` describe the destination document as
//! of the moment of the call. An out-of-range line or column is clamped
//! rather than rejected with a typed failure, matching real VS Code:
//! `TextModel`'s `validatePosition` (`vs/editor/common/model/*`,
//! `PieceTreeTextBuffer.validatePosition`) clamps a `Position` whose line
//! exceeds the model's line count to the last line, and whose column
//! exceeds that line's length to the line's end -- it never throws or
//! silently drops the request. A Problems entry can easily outlive the
//! exact document snapshot the diagnostic was computed against (the file was
//! edited or reloaded between diagnostic and double-click), so this adapter
//! reproduces that clamp instead of making a marker un-activatable the
//! moment the document drifts by one line. `totalLineCount == 0` clamps to
//! line 0, column 0, matching `validatePosition` on an empty model.
//!
//! A clamped column additionally never splits a UTF-16 surrogate pair: if
//! the requested (or line-end-clamped) column would land strictly between a
//! high surrogate and its low surrogate, the column moves back one unit, in
//! front of the pair. This mirrors `validatePosition`'s own surrogate
//! safety check, which exists because Win32 and VS Code alike must never
//! hand a text-rendering/caret API a position that bisects one code point.
[[nodiscard]] ConvertedLogicPosition ConvertMarkerPositionToLogicPoint(
	std::uint32_t zeroBasedLine,
	std::uint32_t zeroBasedUtf16Column,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent);

//! Convenience wrapper converting only a `MarkerRange`'s start position --
//! the position the Problems panel's double-click activation needs to place
//! the caret at.
[[nodiscard]] ConvertedLogicPosition ConvertMarkerRangeStartToLogicPoint(
	const MarkerRange& range,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent);

//! Converts both endpoints of a `MarkerRange`, for callers that want to
//! select the diagnostic's full span rather than only placing the caret at
//! its start.
[[nodiscard]] ConvertedLogicRange ConvertMarkerRangeToLogicRange(
	const MarkerRange& range,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent);

} // namespace workbench::problems
