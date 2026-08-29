/*! @file @brief Bounded text extraction from the parsed terminal model. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"
#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace terminal {

struct TerminalCaptureExtractionRequest final {
	std::optional<std::int64_t> startLine;
	std::optional<std::int64_t> endLine;
	//! When present, extraction visits only these model coordinates. An empty
	//! selection is a successful empty delta. Range filtering happens before
	//! any text is copied or serialized.
	std::optional<std::vector<TerminalRowRange>> selectedRanges;
	bool joinWrappedLines{};
	TerminalCaptureLimits limits;
	std::chrono::steady_clock::time_point deadline{};
};

struct TerminalCaptureExtractionResult final {
	TerminalCaptureResultCode code{ TerminalCaptureResultCode::InvalidRequest };
	std::vector<TerminalCapturedLine> lines;
	bool alternateScreen{};
	bool truncated{};
	TerminalCaptureTruncationReason truncationReason{ TerminalCaptureTruncationReason::None };
	std::size_t physicalRowsVisited{};
	std::size_t codeUnits{};
	std::size_t utf8Bytes{};
};

//! Extracts only the requested current-model rows. No raw PTY bytes, renderer,
//! HWND, or diagnostic trace participates in this operation.
[[nodiscard]] TerminalCaptureExtractionResult ExtractTerminalCapture(
	const TerminalModel& model,
	const TerminalCaptureExtractionRequest& request );

} // namespace terminal
