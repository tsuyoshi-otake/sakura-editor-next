/*! @file
 * @brief Port of VS Code `markerNavigationService.ts` `MarkerList`.
 *
 * Owns the sorted marker run plus move/init/reset semantics used by the
 * `editor.action.marker.*` commands. Pure: no HWND, no document, no I/O.
 * The word-expansion provider and the position conversion live outside;
 * landing a selection goes through `MarkerPositionAdapter` at the native seam.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/problems/MarkerService.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::problems {

//! `problems.sortOrder` の設定値。VS Code の既定は `severity`。
enum class EMarkerSortOrder : std::uint8_t {
	Severity,
	Position,
};

//! `problems.sortOrder` 文字列を読む。未知値は upstream と同じく severity 既定に倒す。
[[nodiscard]] EMarkerSortOrder MarkerSortOrderFromSetting(std::wstring_view value) noexcept;

//! 0 始まり UTF-16 のキャレット位置。upstream の Position(1-based) と比較順序は同一。
struct MarkerNavigationPosition final {
	std::uint32_t line{};
	std::uint32_t column{};

	[[nodiscard]] bool operator==(const MarkerNavigationPosition&) const noexcept = default;
};

//! ナビゲーション走査用に平坦化した 1 マーカー。upstream `IMarker` 相当。
struct MarkerNavigationMarker final {
	//! upstream `resource.toString()`。フィルタ・ソートのキー。
	std::wstring resourceUri;
	MarkerRange range;
	EMarkerSeverity severity{ EMarkerSeverity::Error };
	std::wstring message;

	[[nodiscard]] bool operator==(const MarkerNavigationMarker&) const noexcept = default;
};

//! upstream Range.containsPosition: 両端を含む位置包含判定。
[[nodiscard]] bool MarkerNavigationRangeContainsPosition(
	const MarkerRange& range, MarkerNavigationPosition position) noexcept;

//! upstream Range.isEmpty: start == end。
[[nodiscard]] bool MarkerNavigationRangeIsEmpty(const MarkerRange& range) noexcept;

//! upstream `markerService.read({ resource, severities: Error|Warning|Info })`
//! + `compareMarker` ソート。resourceUriFilter は extUri 等値で照合する。
[[nodiscard]] std::vector<MarkerNavigationMarker> BuildMarkerNavigationMarkers(
	const ProblemsSnapshot& snapshot,
	const std::optional<std::wstring>& resourceUriFilter,
	EMarkerSortOrder order);

/*!
	@brief `MarkerList` の移植。ソート済み走査配列と `_nextIdx` 遷移だけを持つ。

	サブスクリプションは持たない。`SetMarkers` が upstream の
	`updateMarker()` + `_nextIdx = -1` をまとめて行い、内容が同じなら
	現在位置を保持する。
*/
class CMarkerNavigationModel final {
public:
	//! upstream `model.getWordAtPosition(position)` の注入形。
	//! 戻り値は同一行内の (startColumn, endColumn)。
	using WordAtPosition = std::function<std::optional<std::pair<std::uint32_t, std::uint32_t>>(
		MarkerNavigationPosition)>;

	//! upstream `MarkerCoordinate`: marker index, 1-based ordinal, total。
	struct MarkerCoordinate final {
		std::size_t markerIndex{};
		std::size_t ordinal{};
		std::size_t total{};

		[[nodiscard]] bool operator==(const MarkerCoordinate&) const noexcept = default;
	};

	//! `markers` は未ソートでよい。内部で upstream 順に並べる。
	CMarkerNavigationModel(std::optional<std::wstring> resourceUriFilter,
		std::vector<MarkerNavigationMarker> markers, EMarkerSortOrder order);

	//! upstream `matches(uri)`。フィルタ無しモデルはフィルタ無しの
	//! 問い合わせ (nullopt) にのみ一致し、URI フィルタ付きは
	//! `toString() ===` の文字列等値で照合する。片方だけが URI を持つ
	//! 組み合わせは upstream と同じく不一致。引数は canonical 形。
	[[nodiscard]] bool Matches(const std::optional<std::wstring>& resourceUri) const;

	//! upstream `updateMarker()`。並べ替えて内容比較し、変化時のみ差し替えて
	//! `_nextIdx = -1` にする。戻り値は内容が変わったか。
	bool SetMarkers(std::vector<MarkerNavigationMarker> markers);

	//! upstream `move(fwd, model, position)`。選択 index が動いたかを返す。
	//! `modelUri` は `model.uri.toString()`。
	bool Move(bool forward, std::wstring_view modelUri,
		MarkerNavigationPosition position, const WordAtPosition& wordAtPosition);

	//! upstream `selected`。index が範囲外(末尾越え)のときは空。
	[[nodiscard]] std::optional<MarkerCoordinate> Selected() const noexcept;
	[[nodiscard]] const MarkerNavigationMarker& MarkerAt(std::size_t index) const noexcept;
	[[nodiscard]] std::size_t MarkerCount() const noexcept { return m_markers.size(); }

	//! upstream `resetIndex()`。
	void ResetIndex() noexcept { m_nextIndex = -1; }

private:
	void InitIndex(std::wstring_view modelUri, MarkerNavigationPosition position,
		bool forward, const WordAtPosition& wordAtPosition);

	std::optional<std::wstring> m_resourceUriFilter;
	EMarkerSortOrder m_order;
	std::vector<MarkerNavigationMarker> m_markers;
	std::ptrdiff_t m_nextIndex = -1;
};

} // namespace workbench::problems
