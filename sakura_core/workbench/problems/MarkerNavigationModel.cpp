/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/problems/MarkerNavigationModel.h"

#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string_view>

namespace workbench::problems {
namespace {

//! upstream `strings.compare`: UTF-16 コード単位の序数比較。
[[nodiscard]] int CompareStrings(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left < right) return -1;
	if (left > right) return 1;
	return 0;
}

//! upstream `Range.compareRangesUsingStarts`。
[[nodiscard]] int CompareRangesUsingStarts(const MarkerRange& left, const MarkerRange& right) noexcept
{
	if (left.startLine != right.startLine) return left.startLine < right.startLine ? -1 : 1;
	if (left.startColumn != right.startColumn) return left.startColumn < right.startColumn ? -1 : 1;
	if (left.endLine != right.endLine) return left.endLine < right.endLine ? -1 : 1;
	if (left.endColumn != right.endColumn) return left.endColumn < right.endColumn ? -1 : 1;
	return 0;
}

//! upstream `MarkerSeverity.compare`。本 enum の並びが既に Error<Warning<Info<Hint。
[[nodiscard]] int CompareSeverity(EMarkerSeverity left, EMarkerSeverity right) noexcept
{
	const int l = static_cast<int>(left), r = static_cast<int>(right);
	if (l != r) return l < r ? -1 : 1;
	return 0;
}

//! upstream `isEqual` (`extUri.isEqual`)。両方解析できれば URI 等値、
//! 片方でも解析不能なら文字列等値に倒す。
[[nodiscard]] bool IsResourceEqual(std::wstring_view left, std::wstring_view right)
{
	const auto leftUri = platform::uri::Uri::Parse(left);
	const auto rightUri = platform::uri::Uri::Parse(right);
	if (leftUri.value && rightUri.value) {
		return platform::uri::UriIdentityService::IsEqual(*leftUri.value, *rightUri.value);
	}
	return left == right;
}

//! upstream では `model.uri.toString()` が常に URI の canonical 形を返すので
//! 順序比較と `!==` 判定はその形同士で行われる。こちら側の呼び出し側文字列は
//! `Uri::ToString()` 以外の綴り (生の %XX 形式など) も来得るため、
//! `_initIdx` 入口で 1 回だけ canonical 形へ揃える。解析不能なら原文のまま。
[[nodiscard]] std::wstring CanonicalResourceUri(std::wstring_view uri)
{
	const auto parsed = platform::uri::Uri::Parse(uri);
	if (parsed.value) return parsed.value->ToString();
	return std::wstring(uri);
}

//! upstream `compareMarker` (`problems.sortOrder` 反映済み)。
[[nodiscard]] int CompareMarkers(const MarkerNavigationMarker& left,
	const MarkerNavigationMarker& right, EMarkerSortOrder order) noexcept
{
	int result = CompareStrings(left.resourceUri, right.resourceUri);
	if (result == 0) {
		if (order == EMarkerSortOrder::Position) {
			result = CompareRangesUsingStarts(left.range, right.range);
			if (result == 0) result = CompareSeverity(left.severity, right.severity);
		} else {
			result = CompareSeverity(left.severity, right.severity);
			if (result == 0) result = CompareRangesUsingStarts(left.range, right.range);
		}
	}
	return result;
}

void SortMarkers(std::vector<MarkerNavigationMarker>& markers, EMarkerSortOrder order)
{
	std::stable_sort(markers.begin(), markers.end(),
		[order](const auto& left, const auto& right) {
			return CompareMarkers(left, right, order) < 0;
		});
}

//! upstream `updateMarker` の `equals` 比較項目: resource/range/severity/message。
[[nodiscard]] bool SameNavigationMarkerContent(const MarkerNavigationMarker& left,
	const MarkerNavigationMarker& right) noexcept
{
	return left.resourceUri == right.resourceUri
		&& left.range == right.range
		&& left.severity == right.severity
		&& left.message == right.message;
}

[[nodiscard]] std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return L"\xfffd";
	const auto sourceLength = static_cast<int>(value.size());
	const int characterCount = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), sourceLength, nullptr, 0);
	if (characterCount <= 0) return L"\xfffd";
	std::wstring result(static_cast<std::size_t>(characterCount), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength,
		result.data(), characterCount) != characterCount) {
		return L"\xfffd";
	}
	return result;
}

//! upstream `position.isBeforeOrEqual(range.getStartPosition())`。
[[nodiscard]] bool IsBeforeOrEqual(MarkerNavigationPosition position,
	const MarkerRange& range) noexcept
{
	if (position.line != range.startLine) return position.line < range.startLine;
	return position.column <= range.startColumn;
}

} // namespace

EMarkerSortOrder MarkerSortOrderFromSetting(std::wstring_view value) noexcept
{
	if (value == L"position") return EMarkerSortOrder::Position;
	return EMarkerSortOrder::Severity;
}

bool MarkerNavigationRangeContainsPosition(
	const MarkerRange& range, MarkerNavigationPosition position) noexcept
{
	if (position.line < range.startLine || position.line > range.endLine) return false;
	if (position.line == range.startLine && position.column < range.startColumn) return false;
	if (position.line == range.endLine && position.column > range.endColumn) return false;
	return true;
}

bool MarkerNavigationRangeIsEmpty(const MarkerRange& range) noexcept
{
	return range.startLine == range.endLine && range.startColumn == range.endColumn;
}

std::vector<MarkerNavigationMarker> BuildMarkerNavigationMarkers(
	const ProblemsSnapshot& snapshot,
	const std::optional<std::wstring>& resourceUriFilter,
	EMarkerSortOrder order)
{
	std::vector<MarkerNavigationMarker> markers;
	for (const auto& resource : snapshot.resources) {
		const auto resourceUri = resource.resource.ToString();
		// upstream `read({resource})` は extUri 等値で絞る。
		if (resourceUriFilter && !IsResourceEqual(resourceUri, *resourceUriFilter)) continue;
		for (const auto& marker : resource.markers) {
			// upstream の severities フィルタは Error|Warning|Info。Hint は対象外。
			if (marker.severity == EMarkerSeverity::Hint) continue;
			markers.push_back(MarkerNavigationMarker{
				.resourceUri = resourceUri,
				.range = marker.range,
				.severity = marker.severity,
				.message = Utf8ToWide(marker.message),
			});
		}
	}
	SortMarkers(markers, order);
	return markers;
}

CMarkerNavigationModel::CMarkerNavigationModel(
	std::optional<std::wstring> resourceUriFilter,
	std::vector<MarkerNavigationMarker> markers, EMarkerSortOrder order)
	: m_resourceUriFilter(std::move(resourceUriFilter))
	, m_order(order)
{
	SetMarkers(std::move(markers));
}

bool CMarkerNavigationModel::Matches(const std::optional<std::wstring>& resourceUri) const
{
	// upstream `matches(uri)`:
	//   !filter && !uri  -> true / !filter || !uri -> false / else filter(uri)。
	// フィルタ無しモデル (inFiles) はフィルタ無しの問い合わせにのみ一致する。
	// URI フィルタは `uri.toString() === filter.toString()` の文字列等値
	// (isEqual ではない)。両側とも canonical `Uri::ToString()` 形を要求する。
	if (!m_resourceUriFilter && !resourceUri) return true;
	if (!m_resourceUriFilter || !resourceUri) return false;
	return *m_resourceUriFilter == *resourceUri;
}

bool CMarkerNavigationModel::SetMarkers(std::vector<MarkerNavigationMarker> markers)
{
	SortMarkers(markers, m_order);
	if (markers.size() == m_markers.size()
		&& std::equal(markers.begin(), markers.end(), m_markers.begin(), SameNavigationMarkerContent)) {
		return false;
	}
	m_markers = std::move(markers);
	// upstream onMarkerChanged -> updateMarker() == true -> _nextIdx = -1。
	m_nextIndex = -1;
	return true;
}

void CMarkerNavigationModel::InitIndex(std::wstring_view modelUri,
	MarkerNavigationPosition position, bool forward, const WordAtPosition& wordAtPosition)
{
	const auto size = static_cast<std::ptrdiff_t>(m_markers.size());
	// upstream の比較対象は常に `model.uri.toString()` の canonical 形。
	const std::wstring canonicalUri = CanonicalResourceUri(modelUri);
	// upstream `findIndex(marker => isEqual(marker.resource, model.uri))`。
	std::ptrdiff_t index = -1;
	for (std::size_t i = 0; i < m_markers.size(); ++i) {
		if (IsResourceEqual(m_markers[i].resourceUri, canonicalUri)) {
			index = static_cast<std::ptrdiff_t>(i);
			break;
		}
	}

	if (index < 0) {
		// upstream: binarySearch2 で `model.uri.toString()` の挿入位置を取る。
		// isEqual で見つからなかった以上、文字列一致は起こり得ないので挿入位置のみ。
		std::size_t low = 0, high = m_markers.size();
		while (low < high) {
			const std::size_t mid = low + (high - low) / 2;
			if (CompareStrings(m_markers[mid].resourceUri, canonicalUri) < 0) low = mid + 1;
			else high = mid;
		}
		index = static_cast<std::ptrdiff_t>(low);
		if (forward) {
			m_nextIndex = index;
		} else {
			m_nextIndex = (size + index - 1) % size;
		}
	} else {
		bool found = false;
		bool wentPast = false;
		for (std::size_t i = static_cast<std::size_t>(index); i < m_markers.size(); ++i) {
			MarkerRange range = m_markers[i].range;
			// upstream は空レンジを model.getWordAtPosition で単語へ拡げる。
			if (MarkerNavigationRangeIsEmpty(range) && wordAtPosition) {
				if (const auto word = wordAtPosition({ range.startLine, range.startColumn })) {
					range.startColumn = word->first;
					range.endLine = range.startLine;
					range.endColumn = word->second;
				}
			}
			const bool contains = MarkerNavigationRangeContainsPosition(range, position);
			if (contains || IsBeforeOrEqual(position, range)) {
				m_nextIndex = static_cast<std::ptrdiff_t>(i);
				found = true;
				wentPast = !contains;
				break;
			}
			// upstream は位置判定の後で別リソースへ出た時点で走査を打ち切る。
			if (m_markers[i].resourceUri != canonicalUri) break;
		}

		if (!found) {
			// upstream: そのファイルの最後の変更を過ぎた → fwd なら先頭へ巻き戻し。
			m_nextIndex = forward ? 0 : size - 1;
		} else if (wentPast && !forward) {
			m_nextIndex -= 1;
		}
	}

	if (m_nextIndex < 0) {
		m_nextIndex = size - 1;
	}
}

bool CMarkerNavigationModel::Move(bool forward, std::wstring_view modelUri,
	MarkerNavigationPosition position, const WordAtPosition& wordAtPosition)
{
	if (m_markers.empty()) return false;

	const auto oldIndex = m_nextIndex;
	const auto size = static_cast<std::ptrdiff_t>(m_markers.size());
	if (m_nextIndex == -1) {
		InitIndex(modelUri, position, forward, wordAtPosition);
	} else if (forward) {
		m_nextIndex = (m_nextIndex + 1) % size;
	} else {
		m_nextIndex = (m_nextIndex - 1 + size) % size;
	}
	return oldIndex != m_nextIndex;
}

std::optional<CMarkerNavigationModel::MarkerCoordinate> CMarkerNavigationModel::Selected() const noexcept
{
	// upstream `_markers[this._nextIdx]` は末尾越えで undefined。
	if (m_nextIndex < 0 || static_cast<std::size_t>(m_nextIndex) >= m_markers.size()) {
		return std::nullopt;
	}
	return MarkerCoordinate{
		.markerIndex = static_cast<std::size_t>(m_nextIndex),
		.ordinal = static_cast<std::size_t>(m_nextIndex) + 1,
		.total = m_markers.size(),
	};
}

const MarkerNavigationMarker& CMarkerNavigationModel::MarkerAt(std::size_t index) const noexcept
{
	return m_markers[index];
}

} // namespace workbench::problems
