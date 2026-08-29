/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/runtime/TerminalCaptureIndex.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace terminal {

TerminalCaptureIndex::TerminalCaptureIndex(
	TerminalRuntimeGeneration runtimeGeneration,
	TerminalInstanceId instanceId,
	std::uint64_t instanceGeneration,
	std::uint64_t screenEpoch,
	TerminalContentRevision initialRevision,
	std::uint64_t scrollbackBaseOrdinal,
	TerminalCaptureIndexLimits limits)
	: m_runtimeGeneration(runtimeGeneration)
	, m_instanceId(instanceId)
	, m_instanceGeneration(instanceGeneration)
	, m_screenEpoch(screenEpoch)
	, m_currentRevision(initialRevision)
	, m_scrollbackBaseOrdinal(scrollbackBaseOrdinal)
	, m_limits(limits)
{
}

bool TerminalCaptureIndex::NormalizeRanges(
	std::vector<TerminalRowRange>& ranges,
	std::size_t maximumRanges)
{
	if (ranges.size() > maximumRanges) return false;
	for (const auto& range : ranges) {
		if (range.first > range.last) return false;
	}
	std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
		return left.first < right.first || (left.first == right.first && left.last < right.last);
	});
	std::vector<TerminalRowRange> normalized;
	normalized.reserve(ranges.size());
	for (const auto& range : ranges) {
		if (normalized.empty()
			|| (normalized.back().last != (std::numeric_limits<std::int64_t>::max)()
				&& range.first > normalized.back().last + 1)) {
			normalized.push_back(range);
		} else {
			normalized.back().last = (std::max)(normalized.back().last, range.last);
		}
	}
	ranges = std::move(normalized);
	return true;
}

ETerminalCaptureIndexRecordCode TerminalCaptureIndex::Record(TerminalChangeRecord change)
{
	if (!m_runtimeGeneration.IsValid() || !m_instanceId.IsValid() || m_instanceGeneration == 0
		|| m_screenEpoch == 0 || !m_currentRevision.IsValid() || m_limits.maximumRecords == 0) {
		return ETerminalCaptureIndexRecordCode::InvalidRecord;
	}
	if (m_currentRevision.value == (std::numeric_limits<std::uint64_t>::max)()) {
		return ETerminalCaptureIndexRecordCode::RevisionExhausted;
	}
	if (change.revision.value != m_currentRevision.value + 1 || change.screenEpoch != m_screenEpoch) {
		return ETerminalCaptureIndexRecordCode::InvalidRecord;
	}
	if (!NormalizeRanges(change.dirtyScreenRanges, m_limits.maximumRangesPerRecord)) {
		return change.dirtyScreenRanges.size() > m_limits.maximumRangesPerRecord
			? ETerminalCaptureIndexRecordCode::ResourceExhausted
			: ETerminalCaptureIndexRecordCode::InvalidRecord;
	}
	if ((change.appendedHistoryBeginOrdinal == 0) != (change.appendedHistoryEndOrdinal == 0)
		|| (change.appendedHistoryBeginOrdinal != 0
			&& change.appendedHistoryBeginOrdinal > change.appendedHistoryEndOrdinal)) {
		return ETerminalCaptureIndexRecordCode::InvalidRecord;
	}

	auto nextScrollbackBaseOrdinal = m_scrollbackBaseOrdinal;
	if (change.evictedThroughOrdinal != 0
		&& change.evictedThroughOrdinal >= nextScrollbackBaseOrdinal) {
		if (change.evictedThroughOrdinal == (std::numeric_limits<std::uint64_t>::max)()) {
			return ETerminalCaptureIndexRecordCode::RevisionExhausted;
		}
		nextScrollbackBaseOrdinal = change.evictedThroughOrdinal + 1;
	}
	m_currentRevision = change.revision;
	m_scrollbackBaseOrdinal = nextScrollbackBaseOrdinal;
	m_records.push_back(std::move(change));
	while (m_records.size() > m_limits.maximumRecords) m_records.pop_front();
	return ETerminalCaptureIndexRecordCode::Succeeded;
}

ETerminalCaptureIndexRecordCode TerminalCaptureIndex::ResetScreen(
	std::uint64_t newScreenEpoch,
	std::uint64_t scrollbackBaseOrdinal)
{
	if (newScreenEpoch == 0 || newScreenEpoch == m_screenEpoch
		|| m_currentRevision.value == (std::numeric_limits<std::uint64_t>::max)()
		|| m_limits.maximumRecords == 0) {
		return ETerminalCaptureIndexRecordCode::InvalidRecord;
	}
	m_screenEpoch = newScreenEpoch;
	m_scrollbackBaseOrdinal = scrollbackBaseOrdinal;
	m_records.clear();
	TerminalChangeRecord change;
	change.revision = TerminalContentRevision{ m_currentRevision.value + 1 };
	change.screenEpoch = newScreenEpoch;
	change.fullInvalidation = true;
	m_currentRevision = change.revision;
	m_records.push_back(std::move(change));
	return ETerminalCaptureIndexRecordCode::Succeeded;
}

bool TerminalCaptureIndex::SameInstance(const TerminalCaptureCursor& cursor) const noexcept
{
	return cursor.version == 1
		&& cursor.runtimeGeneration == m_runtimeGeneration
		&& cursor.instanceId == m_instanceId
		&& cursor.instanceGeneration == m_instanceGeneration;
}

TerminalCaptureCursor TerminalCaptureIndex::MakeCursor(TerminalContentRevision revision) const noexcept
{
	TerminalCaptureCursor cursor;
	cursor.version = 1;
	cursor.runtimeGeneration = m_runtimeGeneration;
	cursor.instanceId = m_instanceId;
	cursor.instanceGeneration = m_instanceGeneration;
	cursor.screenEpoch = m_screenEpoch;
	cursor.revision = revision;
	cursor.scrollbackBaseOrdinal = m_scrollbackBaseOrdinal;
	return cursor;
}

TerminalCaptureCursor TerminalCaptureIndex::CurrentCursor() const noexcept
{
	return MakeCursor(m_currentRevision);
}

TerminalCaptureCursor TerminalCaptureIndex::EarliestCursor() const noexcept
{
	if (m_records.empty()) return CurrentCursor();
	const auto first = m_records.front().revision.value;
	return MakeCursor(TerminalContentRevision{ first == 0 ? 0 : first - 1 });
}

TerminalCaptureDelta TerminalCaptureIndex::ChangesSince(const TerminalCaptureCursor& cursor) const
{
	TerminalCaptureDelta result;
	result.earliestCursor = EarliestCursor();
	result.nextCursor = CurrentCursor();
	if (!SameInstance(cursor) || cursor.revision.value > m_currentRevision.value) return result;
	if (cursor.screenEpoch != m_screenEpoch) {
		result.code = ETerminalCaptureDeltaCode::Gap;
		result.gap = true;
		result.resyncSnapshot = true;
		return result;
	}
	if (cursor.scrollbackBaseOrdinal > m_scrollbackBaseOrdinal) return result;
	if (cursor.scrollbackBaseOrdinal < m_scrollbackBaseOrdinal) {
		result.code = ETerminalCaptureDeltaCode::Gap;
		result.gap = true;
		result.resyncSnapshot = true;
		return result;
	}
	if (cursor.revision == m_currentRevision) {
		result.code = ETerminalCaptureDeltaCode::UpToDate;
		return result;
	}
	if (cursor.revision.value < result.earliestCursor.revision.value) {
		result.code = ETerminalCaptureDeltaCode::Gap;
		result.gap = true;
		result.resyncSnapshot = true;
		return result;
	}

	for (const auto& record : m_records) {
		if (record.revision.value <= cursor.revision.value) continue;
		if (record.fullInvalidation) {
			result.code = ETerminalCaptureDeltaCode::Gap;
			result.gap = true;
			result.resyncSnapshot = true;
			result.dirtyScreenRanges.clear();
			return result;
		}
		result.dirtyScreenRanges.insert(result.dirtyScreenRanges.end(),
			record.dirtyScreenRanges.begin(), record.dirtyScreenRanges.end());
		if (record.appendedHistoryBeginOrdinal != 0) {
			if (result.appendedHistoryBeginOrdinal == 0) {
				result.appendedHistoryBeginOrdinal = record.appendedHistoryBeginOrdinal;
			} else if (result.appendedHistoryEndOrdinal == (std::numeric_limits<std::uint64_t>::max)()
				|| result.appendedHistoryEndOrdinal + 1 != record.appendedHistoryBeginOrdinal) {
				result.code = ETerminalCaptureDeltaCode::Gap;
				result.gap = true;
				result.resyncSnapshot = true;
				result.dirtyScreenRanges.clear();
				result.appendedHistoryBeginOrdinal = 0;
				result.appendedHistoryEndOrdinal = 0;
				return result;
			}
			result.appendedHistoryEndOrdinal = record.appendedHistoryEndOrdinal;
		}
	}
	const auto maximumCombinedRanges = m_limits.maximumRangesPerRecord != 0
		&& m_limits.maximumRecords > (std::numeric_limits<std::size_t>::max)() / m_limits.maximumRangesPerRecord
		? (std::numeric_limits<std::size_t>::max)()
		: m_limits.maximumRecords * m_limits.maximumRangesPerRecord;
	if (!NormalizeRanges(result.dirtyScreenRanges, maximumCombinedRanges)) {
		result.code = ETerminalCaptureDeltaCode::Gap;
		result.gap = true;
		result.resyncSnapshot = true;
		result.dirtyScreenRanges.clear();
		return result;
	}
	result.code = ETerminalCaptureDeltaCode::Delta;
	return result;
}

} // namespace terminal
