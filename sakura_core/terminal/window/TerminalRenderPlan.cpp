/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalRenderPlan.h"

#include <algorithm>
#include <limits>
#include <new>

namespace terminal {
namespace {

[[nodiscard]] bool IsSelected(TerminalSelectionPoint point, TerminalSelectionPoint anchor,
	TerminalSelectionPoint active) noexcept
{
	const auto less = [](const TerminalSelectionPoint& left, const TerminalSelectionPoint& right) {
		return left.row < right.row || (left.row == right.row && left.column < right.column);
	};
	if( less(active, anchor) ) std::swap(anchor, active);
	return !less(point, anchor) && less(point, active);
}

[[nodiscard]] LONG PixelCoordinate(std::size_t cell, int cellExtent) noexcept
{
	const auto extent = std::max(1, cellExtent);
	const auto product = static_cast<unsigned long long>(cell) * static_cast<unsigned long long>(extent);
	return product > static_cast<unsigned long long>(std::numeric_limits<LONG>::max())
		? std::numeric_limits<LONG>::max()
		: static_cast<LONG>(product);
}

[[nodiscard]] bool Intersects(const RECT& left, const RECT& right) noexcept
{
	return left.left < right.right && left.right > right.left && left.top < right.bottom && left.bottom > right.top;
}

[[nodiscard]] bool SameRectRow(const RECT& left, const RECT& right) noexcept
{
	return left.top == right.top && left.bottom == right.bottom && left.right == right.left;
}

} // namespace

bool TerminalRenderPlan::Build(const TerminalRenderPlanBuildInput& input)
{
	Clear();
	if( input.model == nullptr || input.classifier == nullptr || input.styleResolver == nullptr ||
		input.paintRect.right <= input.paintRect.left || input.paintRect.bottom <= input.paintRect.top ) return false;

	const auto effectiveCellWidth = std::max(1, input.cellWidth);
	const auto effectiveCellHeight = std::max(1, input.cellHeight);
	const auto paintTop = std::max<LONG>(0, input.paintRect.top);
	const auto paintBottom = std::max<LONG>(0, input.paintRect.bottom);
	const auto gridTop = std::max(0, static_cast<int>(paintTop) - input.geometry.GridOriginY());
	const auto gridBottom = std::max(0, static_cast<int>(paintBottom) - input.geometry.GridOriginY());
	const auto firstVisible = std::min<std::size_t>(input.viewport.visibleRows,
		static_cast<std::size_t>(gridTop / effectiveCellHeight));
	const auto lastVisible = std::min<std::size_t>(input.viewport.visibleRows,
		static_cast<std::size_t>((gridBottom + effectiveCellHeight - 1) / effectiveCellHeight));
	const auto visibleRows = lastVisible >= firstVisible ? lastVisible - firstVisible : 0;
	const auto columns = input.model->Columns();
	if( columns != 0 && visibleRows > kMaximumVisibleCells / columns ) {
		// The product is intentionally not evaluated here: it exceeds the plan's
		// hard limit and can itself overflow size_t for malformed geometry.  The
		// maximum value is the deterministic saturated diagnostic for that case.
		m_counters.rejectedViewportCells = std::numeric_limits<std::size_t>::max();
		return false;
	}
	const auto visibleCells = visibleRows * columns;
	if( visibleCells > kMaximumVisibleCells || !ReserveForViewport(visibleCells) ) {
		m_counters.rejectedViewportCells = visibleCells;
		return false;
	}
	m_counters.classifierGeneration = input.classifier->Generation();
	// Terminal rows overwhelmingly repeat one resolved default style.  Keep only
	// the immediately preceding key/result on this Build stack frame: it avoids
	// resolver work without retaining palette, font, or selection state across
	// frames, and preserves an exact resolver call at every key transition.
	TerminalAttributes previousAttributes{};
	TerminalRenderStyle previousStyle{};
	bool previousSelected{};
	bool hasPreviousStyle{};

	for( std::size_t visualRow = firstVisible; visualRow < lastVisible; ++visualRow ) {
		const auto globalRow = input.viewport.topRow + visualRow;
		const auto* row = GetTerminalRow(*input.model, globalRow);
		if( row == nullptr ) continue;
		// A terminal row normally has exactly Columns() entries.  Keep the plan
		// bounded by model geometry even while a row is being reconstructed after
		// a resize, rather than allowing malformed backing storage to grow a frame.
		const auto rowColumns = std::min<std::size_t>(row->cells.size(), columns);
		for( std::size_t column = 0; column < rowColumns; ) {
			const auto& cell = row->cells[column];
			if( cell.continuation ) {
				++column;
				continue;
			}
			++m_counters.visibleCellsScanned;
			const auto occupiedColumns = std::max<std::size_t>(1, cell.width);
			const RECT rect{
				input.geometry.GridOriginX() + PixelCoordinate(column, effectiveCellWidth),
				input.geometry.GridOriginY() + PixelCoordinate(visualRow, effectiveCellHeight),
				input.geometry.GridOriginX() + PixelCoordinate(column + occupiedColumns, effectiveCellWidth),
				input.geometry.GridOriginY() + PixelCoordinate(visualRow + 1, effectiveCellHeight),
			};
			const auto attributes = row->AttributesAt(column);
			const bool selected = input.hasSelection && IsSelected({ globalRow, column },
				input.selectionAnchor, input.selectionActive);
			const bool reusesPreviousStyle = hasPreviousStyle && previousSelected == selected && previousAttributes == attributes;
			const auto style = reusesPreviousStyle
				? previousStyle
				: input.styleResolver(input.styleResolverContext, attributes, selected);
			if( !reusesPreviousStyle ) {
				previousAttributes = attributes;
				previousSelected = selected;
				previousStyle = style;
				hasPreviousStyle = true;
			}
			const bool intersectsPaint = Intersects(rect, input.paintRect);
			if( intersectsPaint &&
				!(input.surfaceClearedToDefaultBackground && style.usesSurfaceDefaultBackground) ) {
				AppendBackground(rect, style);
			}

			const auto text = cell.Text();
			if( text.empty() || !intersectsPaint ) {
				column += occupiedColumns;
				continue;
			}
			if( IsBuiltinGlyph(text) ) {
				m_builtinGlyphs.push_back({ rect, static_cast<char32_t>(text.front()), style });
				++m_counters.builtinGlyphs;
			} else if( text.size() == 1 && text.front() < 0x80 ) {
				// ASCII is deliberately unconditional: no coverage probe may enter the
				// DirectWrite path simply to classify an ordinary terminal frame.
				AppendGdi(rect, text.front(), static_cast<int>(occupiedColumns * effectiveCellWidth), style);
			} else if( IsSyntacticallyComplex(text) || text.front() == 0x23f5 ||
				input.classifier->Classify(text, style.bold) == TerminalRenderClassification::ShapedFallback ) {
				AppendShaped(rect, text, style);
			} else {
				AppendGdi(rect, text.front(), static_cast<int>(occupiedColumns * effectiveCellWidth), style);
			}
			column += occupiedColumns;
		}
	}
	return true;
}

void TerminalRenderPlan::Clear() noexcept
{
	m_backgroundSpans.clear();
	m_builtinGlyphs.clear();
	m_gdiRuns.clear();
	m_shapedClusters.clear();
	m_textStorage.clear();
	m_advanceStorage.clear();
	m_counters = {};
}

std::span<const TerminalRenderBackgroundSpan> TerminalRenderPlan::BackgroundSpans() const noexcept
{
	return m_backgroundSpans;
}

std::span<const TerminalBuiltinGlyphCommand> TerminalRenderPlan::BuiltinGlyphs() const noexcept
{
	return m_builtinGlyphs;
}

std::span<const TerminalGdiRun> TerminalRenderPlan::GdiRuns() const noexcept
{
	return m_gdiRuns;
}

std::span<const TerminalShapedClusterCommand> TerminalRenderPlan::ShapedClusters() const noexcept
{
	return m_shapedClusters;
}

std::wstring_view TerminalRenderPlan::Text(std::size_t offset, std::size_t length) const noexcept
{
	if( offset > m_textStorage.size() || length > m_textStorage.size() - offset ) return {};
	return { m_textStorage.data() + offset, length };
}

std::span<const int> TerminalRenderPlan::Advances(std::size_t offset, std::size_t count) const noexcept
{
	if( offset > m_advanceStorage.size() || count > m_advanceStorage.size() - offset ) return {};
	return { m_advanceStorage.data() + offset, count };
}

const TerminalRenderPlanCounters& TerminalRenderPlan::Counters() const noexcept
{
	return m_counters;
}

std::size_t TerminalRenderPlan::TextCapacity() const noexcept
{
	return m_textStorage.capacity();
}

std::size_t TerminalRenderPlan::AdvanceCapacity() const noexcept
{
	return m_advanceStorage.capacity();
}

std::size_t TerminalRenderPlan::CommandCapacity() const noexcept
{
	return m_backgroundSpans.capacity() + m_builtinGlyphs.capacity() + m_gdiRuns.capacity() + m_shapedClusters.capacity();
}

bool TerminalRenderPlan::IsBuiltinGlyph(std::wstring_view text) noexcept
{
	return text.size() == 1 && text.front() >= 0x2500 && text.front() <= 0x259f;
}

bool TerminalRenderPlan::IsSyntacticallyComplex(std::wstring_view text) noexcept
{
	// TerminalCell stores an already segmented grapheme.  More than one UTF-16
	// unit therefore covers surrogate pairs, combining sequences, variation
	// selectors, flags, and ZWJ sequences without reimplementing model Unicode.
	return text.size() != 1;
}

bool TerminalRenderPlan::ReserveForViewport(std::size_t visibleCells)
{
	if( visibleCells == 0 ) return true;
	if( visibleCells > kMaximumVisibleCells || visibleCells > std::numeric_limits<std::size_t>::max() / TerminalCell::kMaxCodeUnits ) return false;
	try {
		const auto textUnits = visibleCells * TerminalCell::kMaxCodeUnits;
		const auto reserve = [this](auto& storage, std::size_t count) {
			if( storage.capacity() < count ) {
				storage.reserve(count);
				++m_counters.capacityGrowths;
			}
		};
		reserve(m_backgroundSpans, visibleCells);
		reserve(m_builtinGlyphs, visibleCells);
		reserve(m_gdiRuns, visibleCells);
		reserve(m_shapedClusters, visibleCells);
		reserve(m_textStorage, textUnits);
		reserve(m_advanceStorage, visibleCells);
		return true;
	} catch( const std::bad_alloc& ) {
		return false;
	}
}

void TerminalRenderPlan::AppendBackground(const RECT& rect, const TerminalRenderStyle& style)
{
	if( !m_backgroundSpans.empty() ) {
		auto& previous = m_backgroundSpans.back();
		if( previous.style == style && SameRectRow(previous.rect, rect) ) {
			previous.rect.right = rect.right;
			return;
		}
	}
	m_backgroundSpans.push_back({ rect, style });
	++m_counters.backgroundSpans;
}

void TerminalRenderPlan::AppendGdi(const RECT& rect, wchar_t text, int advance, const TerminalRenderStyle& style)
{
	if( !m_gdiRuns.empty() ) {
		auto& previous = m_gdiRuns.back();
		if( previous.style == style && SameRectRow(previous.rect, rect) &&
			previous.textOffset + previous.textLength == m_textStorage.size() &&
			previous.advanceOffset + previous.advanceCount == m_advanceStorage.size() ) {
			m_textStorage.push_back(text);
			m_advanceStorage.push_back(advance);
			previous.rect.right = rect.right;
			++previous.textLength;
			++previous.advanceCount;
			m_counters.textCodeUnits = m_textStorage.size();
			m_counters.advanceCount = m_advanceStorage.size();
			return;
		}
	}
	const auto textOffset = m_textStorage.size();
	const auto advanceOffset = m_advanceStorage.size();
	m_textStorage.push_back(text);
	m_advanceStorage.push_back(advance);
	m_gdiRuns.push_back({ rect, style, textOffset, 1, advanceOffset, 1 });
	++m_counters.gdiRuns;
	m_counters.textCodeUnits = m_textStorage.size();
	m_counters.advanceCount = m_advanceStorage.size();
}

void TerminalRenderPlan::AppendShaped(const RECT& rect, std::wstring_view text, const TerminalRenderStyle& style)
{
	const auto offset = m_textStorage.size();
	m_textStorage.insert(m_textStorage.end(), text.begin(), text.end());
	m_shapedClusters.push_back({ rect, style, offset, text.size() });
	++m_counters.shapedClusters;
	m_counters.textCodeUnits = m_textStorage.size();
}

} // namespace terminal
