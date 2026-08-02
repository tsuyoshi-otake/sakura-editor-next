/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalRenderMapping.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace terminal {

//! Classification is intentionally independent of the terminal model.  The
//! caller binds it to its current primary font, DPI, and locale generation.
enum class TerminalRenderClassification : std::uint8_t {
	GdiSimple,
	ShapedFallback,
};

//! Fully resolved paint state.  Commands retain this state rather than a
//! pointer into mutable terminal attributes while a frame is being painted.
struct TerminalRenderStyle {
	COLORREF foreground{};
	COLORREF background{};
	bool bold{};
	bool underline{};
	bool inverse{};
	bool selected{};
	// The resolver may set this only when the resolved background is the
	// caller-declared surface clear, with no selection or inverse override.
	bool usesSurfaceDefaultBackground{};

	friend constexpr bool operator==(const TerminalRenderStyle&, const TerminalRenderStyle&) noexcept = default;
};

struct TerminalRenderBackgroundSpan {
	RECT rect{};
	TerminalRenderStyle style{};
};

struct TerminalBuiltinGlyphCommand {
	RECT rect{};
	char32_t glyph{};
	TerminalRenderStyle style{};
};

//! A simple, primary-face GDI run.  Text and advances are offsets into the
//! plan's reusable contiguous frame storage.
struct TerminalGdiRun {
	RECT rect{};
	TerminalRenderStyle style{};
	std::size_t textOffset{};
	std::size_t textLength{};
	std::size_t advanceOffset{};
	std::size_t advanceCount{};
};

//! A cluster requiring DirectWrite analysis/fallback.  Its rectangle is the
//! model-owned occupied-column rectangle and is never remeasured by DWrite.
struct TerminalShapedClusterCommand {
	RECT rect{};
	TerminalRenderStyle style{};
	std::size_t textOffset{};
	std::size_t textLength{};
};

class ITerminalRenderClassifier {
public:
	virtual ~ITerminalRenderClassifier() = default;
	[[nodiscard]] virtual TerminalRenderClassification Classify(std::wstring_view text, bool bold) noexcept = 0;
	[[nodiscard]] virtual std::uint64_t Generation() const noexcept = 0;
};

using TerminalRenderStyleResolver = TerminalRenderStyle (*)(void* context,
	const TerminalAttributes& attributes, bool selected) noexcept;

struct TerminalRenderPlanBuildInput {
	const TerminalModel* model{};
	TerminalViewport viewport{};
	RECT paintRect{};
	int cellWidth{ 1 };
	int cellHeight{ 1 };
	bool hasSelection{};
	TerminalSelectionPoint selectionAnchor{};
	TerminalSelectionPoint selectionActive{};
	ITerminalRenderClassifier* classifier{};
	TerminalRenderStyleResolver styleResolver{};
	void* styleResolverContext{};
	// The target paint rectangle has already been cleared to the terminal's
	// semantic default background.  This is an opt-in elision contract.
	bool surfaceClearedToDefaultBackground{};
};

struct TerminalRenderPlanCounters {
	std::size_t visibleCellsScanned{};
	std::size_t backgroundSpans{};
	std::size_t builtinGlyphs{};
	std::size_t gdiRuns{};
	std::size_t shapedClusters{};
	std::size_t textCodeUnits{};
	std::size_t advanceCount{};
	std::size_t capacityGrowths{};
	std::size_t rejectedViewportCells{};
	std::uint64_t classifierGeneration{};
};

//! Reusable O(visible-cells) terminal viewport paint plan.  The explicit cap
//! is defensive: no malformed model/window combination may grow a renderer
//! frame store without bound.  Normal viewport dimensions are far below it.
class TerminalRenderPlan final {
public:
	static constexpr std::size_t kMaximumVisibleCells = 1024U * 1024U;

	[[nodiscard]] bool Build(const TerminalRenderPlanBuildInput& input);
	void Clear() noexcept;

	[[nodiscard]] std::span<const TerminalRenderBackgroundSpan> BackgroundSpans() const noexcept;
	[[nodiscard]] std::span<const TerminalBuiltinGlyphCommand> BuiltinGlyphs() const noexcept;
	[[nodiscard]] std::span<const TerminalGdiRun> GdiRuns() const noexcept;
	[[nodiscard]] std::span<const TerminalShapedClusterCommand> ShapedClusters() const noexcept;
	[[nodiscard]] std::wstring_view Text(std::size_t offset, std::size_t length) const noexcept;
	[[nodiscard]] std::span<const int> Advances(std::size_t offset, std::size_t count) const noexcept;
	[[nodiscard]] const TerminalRenderPlanCounters& Counters() const noexcept;
	[[nodiscard]] std::size_t TextCapacity() const noexcept;
	[[nodiscard]] std::size_t AdvanceCapacity() const noexcept;
	[[nodiscard]] std::size_t CommandCapacity() const noexcept;

	[[nodiscard]] static bool IsBuiltinGlyph(std::wstring_view text) noexcept;
	[[nodiscard]] static bool IsSyntacticallyComplex(std::wstring_view text) noexcept;

private:
	[[nodiscard]] bool ReserveForViewport(std::size_t visibleCells);
	void AppendBackground(const RECT& rect, const TerminalRenderStyle& style);
	void AppendGdi(const RECT& rect, wchar_t text, int advance, const TerminalRenderStyle& style);
	void AppendShaped(const RECT& rect, std::wstring_view text, const TerminalRenderStyle& style);

	std::vector<TerminalRenderBackgroundSpan> m_backgroundSpans;
	std::vector<TerminalBuiltinGlyphCommand> m_builtinGlyphs;
	std::vector<TerminalGdiRun> m_gdiRuns;
	std::vector<TerminalShapedClusterCommand> m_shapedClusters;
	std::vector<wchar_t> m_textStorage;
	std::vector<int> m_advanceStorage;
	TerminalRenderPlanCounters m_counters;
};

} // namespace terminal
