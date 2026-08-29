/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace terminal {

struct TerminalChangeRecord final {
	TerminalContentRevision revision;
	std::uint64_t screenEpoch{};
	std::vector<TerminalRowRange> dirtyScreenRanges;
	std::uint64_t appendedHistoryBeginOrdinal{};
	std::uint64_t appendedHistoryEndOrdinal{};
	std::uint64_t evictedThroughOrdinal{};
	bool fullInvalidation{};
};

struct TerminalCaptureIndexLimits final {
	std::size_t maximumRecords{ 256 };
	std::size_t maximumRangesPerRecord{ 4096 };
};

enum class ETerminalCaptureIndexRecordCode : std::uint8_t {
	Succeeded,
	InvalidRecord,
	RevisionExhausted,
	ResourceExhausted,
};

enum class ETerminalCaptureDeltaCode : std::uint8_t {
	UpToDate,
	Delta,
	Gap,
	InvalidCursor,
};

struct TerminalCaptureDelta final {
	ETerminalCaptureDeltaCode code{ ETerminalCaptureDeltaCode::InvalidCursor };
	std::vector<TerminalRowRange> dirtyScreenRanges;
	std::uint64_t appendedHistoryBeginOrdinal{};
	std::uint64_t appendedHistoryEndOrdinal{};
	TerminalCaptureCursor earliestCursor;
	TerminalCaptureCursor nextCursor;
	bool gap{};
	bool resyncSnapshot{};
};

//! Bounded metadata-only journal for context-efficient terminal capture.
//!
//! Text and cells remain owned by TerminalModel. This index records only
//! revisions, row ranges, screen epochs, and scrollback eviction fences.
class TerminalCaptureIndex final {
public:
	TerminalCaptureIndex(
		TerminalRuntimeGeneration runtimeGeneration,
		TerminalInstanceId instanceId,
		std::uint64_t instanceGeneration,
		std::uint64_t screenEpoch,
		TerminalContentRevision initialRevision,
		std::uint64_t scrollbackBaseOrdinal,
		TerminalCaptureIndexLimits limits = {});

	[[nodiscard]] ETerminalCaptureIndexRecordCode Record(TerminalChangeRecord change);
	[[nodiscard]] ETerminalCaptureIndexRecordCode ResetScreen(
		std::uint64_t newScreenEpoch,
		std::uint64_t scrollbackBaseOrdinal);
	[[nodiscard]] TerminalCaptureDelta ChangesSince(const TerminalCaptureCursor& cursor) const;

	[[nodiscard]] TerminalCaptureCursor CurrentCursor() const noexcept;
	[[nodiscard]] TerminalCaptureCursor EarliestCursor() const noexcept;
	[[nodiscard]] std::size_t RetainedRecordCount() const noexcept { return m_records.size(); }

private:
	[[nodiscard]] bool SameInstance(const TerminalCaptureCursor& cursor) const noexcept;
	[[nodiscard]] TerminalCaptureCursor MakeCursor(TerminalContentRevision revision) const noexcept;
	[[nodiscard]] static bool NormalizeRanges(
		std::vector<TerminalRowRange>& ranges,
		std::size_t maximumRanges);

	TerminalRuntimeGeneration m_runtimeGeneration;
	TerminalInstanceId m_instanceId;
	std::uint64_t m_instanceGeneration{};
	std::uint64_t m_screenEpoch{};
	TerminalContentRevision m_currentRevision;
	std::uint64_t m_scrollbackBaseOrdinal{};
	TerminalCaptureIndexLimits m_limits;
	std::deque<TerminalChangeRecord> m_records;
};

} // namespace terminal
