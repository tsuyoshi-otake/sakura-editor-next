/*! @file */
#pragma once

#include "terminal/window/TerminalRenderMapping.h"

#include <optional>

namespace terminal {

//! A supported HTTP(S) target and its half-open range in model coordinates.
//! Other protocols, file paths and OSC 8 labels are not web-link capabilities.
struct TerminalWebLink final {
	std::wstring uri;
	TerminalSelectionPoint start;
	TerminalSelectionPoint end;
	friend bool operator==( const TerminalWebLink&, const TerminalWebLink& ) = default;
};

//! Inspects only the clicked logical line, joining soft wraps but never hard
//! newlines. Both inspected cells and UTF-16 units are bounded by this limit;
//! an incomplete/oversized logical line fails closed rather than opening a prefix.
inline constexpr std::size_t kTerminalLinkScanLimit = 8192;
[[nodiscard]] std::optional<TerminalWebLink> DetectTerminalWebLink(
	const TerminalModel& model, TerminalSelectionPoint point );

} // namespace terminal
